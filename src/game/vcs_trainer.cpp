// See vcs_trainer.h and docs/TRAINER.md.
#include "game/vcs_trainer.h"

#include "debug/rig.h"
#include "game/vcs_freecam.h"
#include "game/vcs_hud.h"
#include "ps2_runtime.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>

namespace vcs::trainer
{
    namespace
    {
        constexpr uint32_t kRamMask = PS2_RAM_MASK;

        // ---- Game addresses (game-internals.md 11, 12) ------------------------------------------------------------
        constexpr uint32_t kPlayerPed = 0x004E4910u;      // CWorld::Players[0].m_pPed
        constexpr uint32_t kScriptSpacePtr = 0x0048741Cu; // 0 until the script engine runs
        constexpr uint32_t kGameTimeMs = 0x004CD104u;     // CTimer::m_snTimeInMilliseconds: stands still while paused
        constexpr uint32_t kClockHours = 0x004CD148u;     // CClock: hours, minutes (bytes)
        constexpr uint32_t kCurrArea = 0x00489F7Cu;       // CGame::currArea (0 outdoors)
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8, set by LOAD_AND_LAUNCH_MISSION_INTERNAL
        constexpr uint32_t kActiveScripts = 0x004CD3C4u;  // CRunningScript active list (+0 next, +0x10 IP)
        constexpr uint32_t kTextTable = 0x0048A2A0u;      // -> -> {entries, count, ...}; entry {u32 UTF-16 text, char key[8]}
        constexpr uint32_t kModelInfos = 0x00486518u;     // -> CModelInfo::ms_modelInfoPtrs
        constexpr uint32_t kModelGameName = 0x158u;       // vehicle model info: the game name / text key (8 chars)
        constexpr uint32_t kStreaming = 0x00489FF8u;      // -> CStreaming; +200 -> the models' streaming info (0x18 each,
                                                          // +0xC load state, 1 = loaded): HAS_MODEL_LOADED's test
        constexpr uint32_t kCameraLooksAtPlayer = 0x006F44D0u + 0x888u; // TheCamera: 1 unless a script camera holds it
        constexpr int kVehicleFirst = 171, kVehicleLast = 280;
        // ped fields
        constexpr uint32_t kPedPosition = 0x30u;
        constexpr uint32_t kPedHealth = 0x4E4u; // float
        constexpr uint32_t kPedVehicle = 0x480u; // the vehicle it is in (0 on foot)
        constexpr uint32_t kPedWeapons = 0x578u; // 10 slots of 0x1C: type, state, ammo in clip, ammo total
        constexpr uint32_t kWeaponSlotSize = 0x1Cu;
        constexpr int kWeaponSlots = 10;
        constexpr uint32_t kPedState = 0x8C4u;   // 1 on foot, 0x37 driving, 0x39 / 0x3A dying / dead
        constexpr uint32_t kCarHealth = 0x278u;  // float, 1000 undamaged (below 250 it burns)

        // ---- Script commands (VCS opcode numbers; game-internals.md 11) -------------------------------------------
        enum : uint16_t
        {
            TERMINATE = 0x0023,
            SET_CHAR_COORDINATES = 0x0044,
            CREATE_CAR = 0x0048,
            GET_CAR_COORDINATES = 0x004D,
            SET_CAR_COORDINATES = 0x004E,
            SET_TIME_OF_DAY = 0x005B,
            ADD_SCORE = 0x0094,
            ALTER_WANTED_LEVEL_NO_DROP = 0x0099,
            CLEAR_WANTED_LEVEL = 0x009B,
            SET_TIME_SCALE = 0x00B8,
            GET_CHAR_HEADING = 0x00CE,
            GET_CAR_HEADING = 0x00D0,
            SET_CAR_HEADING = 0x00D1,
            GIVE_WEAPON_TO_CHAR = 0x0106,
            FORCE_WEATHER_NOW = 0x0109,
            RELEASE_WEATHER = 0x010A,
            SET_CURRENT_CHAR_WEAPON = 0x010B,
            MARK_CAR_AS_NO_LONGER_NEEDED = 0x0113,
            SET_POLICE_IGNORE_PLAYER = 0x012E,
            SET_CHAR_HEALTH = 0x014B,
            SET_CAR_HEALTH = 0x014C,
            CHANGE_CAR_COLOUR = 0x0150,
            REQUEST_MODEL = 0x0160,
            MARK_MODEL_AS_NO_LONGER_NEEDED = 0x0162,
            SET_CHAR_PROOFS = 0x01B1,
            SET_CAR_PROOFS = 0x01B2,
            RESTORE_CAMERA_JUMPCUT = 0x01CF,
            SET_PLAYER_NEVER_GETS_TIRED = 0x01FB,
            SET_PLAYER_FAST_RELOAD = 0x01FC,
            ADD_ARMOUR_TO_CHAR = 0x0213,
            WARP_CHAR_FROM_CAR_TO_COORD = 0x0216,
            WARP_CHAR_INTO_CAR = 0x021C,
            SET_EVERYONE_IGNORE_PLAYER = 0x024A,
            STORE_CAR_CHAR_IS_IN_NO_SAVE = 0x024B, // STORE_CAR_CHAR_IS_IN (0x007B) answers -1 for a car made by CREATE_CAR
            LOAD_SCENE = 0x0253,
            LOAD_AND_LAUNCH_MISSION_INTERNAL = 0x0289,
            SET_UPSIDEDOWN_CAR_NOT_DAMAGED = 0x026D,
            SET_CAR_CAN_BE_DAMAGED = 0x0275,
            GET_OFFSET_FROM_CAR_IN_WORLD_COORDS = 0x027F,
            REMOVE_ALL_CHAR_WEAPONS = 0x02D4,
            SET_CAR_FORWARD_SPEED = 0x02E9,
            SET_AREA_VISIBLE = 0x02EA,
            GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS = 0x02EE,
            SET_CHAR_DROWNS_IN_WATER = 0x02F9,
        };

        // Bytecode for one script (rig.Scm in tools/rig.py is the same in Python): arguments carry a type byte.
        class Scm
        {
        public:
            Scm &op(uint16_t code) { return put(code & 0xFF).put(code >> 8); }
            Scm &i(int32_t v)
            {
                if (v == 0)
                    return put(0x01);
                if (v >= -128 && v < 128)
                    return put(0x07).put(static_cast<uint8_t>(v));
                if (v >= -32768 && v < 32768)
                    return put(0x08).put(v & 0xFF).put((v >> 8) & 0xFF);
                put(0x06);
                for (int k = 0; k < 4; ++k)
                    put((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF);
                return *this;
            }
            Scm &f(float v)
            {
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                put(0x09);
                for (int k = 0; k < 4; ++k)
                    put((bits >> (8 * k)) & 0xFF);
                return *this;
            }
            Scm &b(bool v) { return i(v ? 1 : 0); }
            Scm &player() { return put(0xCF).put(0xA5); } // global 0x2A5: the player (and the player's char)
            Scm &local(int n) { return put(static_cast<uint8_t>(0x0D + n)); }
            Scm &car() { return local(9); } // where the vehicle commands keep the player's vehicle handle
            Scm &storePlayerCar() { return op(STORE_CAR_CHAR_IS_IN_NO_SAVE).player().car(); }
            bool empty() const { return code.empty(); }
            std::vector<uint8_t> finished() const
            {
                std::vector<uint8_t> out = code;
                out.push_back(TERMINATE & 0xFF);
                out.push_back(TERMINATE >> 8);
                return out;
            }

        private:
            Scm &put(uint32_t byte)
            {
                code.push_back(static_cast<uint8_t>(byte));
                return *this;
            }
            std::vector<uint8_t> code;
        };

        // ---- Static tables ------------------------------------------------------------------------------------------
        struct WeaponDef
        {
            int type;
            int model;
            const char *textKey;
            const char *fallback;
            int ammo;
        };
        // Weapon types 0..39 (40 and up make the script hang); models and text keys checked in game (docs/TRAINER.md).
        constexpr WeaponDef kWeapons[] = {
            {1, 331, "WEAP03", "Brass Knuckles", 1},   {2, 332, "WEAP04", "Butterfly Knife", 1},
            {3, 333, "WEAP05", "Golf Club", 1},        {4, 334, "WEAP02", "Nightstick", 1},
            {5, 335, "WEAP06", "Knife", 1},            {6, 336, "WEAP01", "Baseball Bat", 1},
            {7, 337, "WEAP07", "Axe", 1},              {8, 338, "WEAP08", "Gaff Hook", 1},
            {9, 339, "WEAP09", "Machete", 1},          {10, 340, "WEAP10", "Katana", 1},
            {11, 341, "WEAP11", "Chainsaw", 1},        {12, 342, "WEAP13", "Grenades", 25},
            {13, 342, "WEAP15", "Remote Grenades", 25}, {14, 368, "WEAP34", "Land Mines", 25},
            {16, 343, "WEAP14", "Tear Gas", 25},       {17, 344, "WEAP12", "Molotovs", 25},
            {19, 346, "WEAP16", "Pistol", 200},        {20, 347, "WEAP17", "Equalizer", 200},
            {21, 349, "WEAP25", "Shotgun", 100},       {22, 350, "WEAP24", "Spaz Shotgun", 100},
            {23, 351, "WEAP26", "Stubby Shotgun", 100}, {24, 353, "WEAP18", "Micro SMG", 500},
            {25, 354, "WEAP19", "Scorpion", 500},      {26, 355, "WEAP21", "Mac 10", 500},
            {27, 356, "WEAP20", "SMG", 500},           {28, 352, "WEAP23", "Assault Rifle", 500},
            {29, 348, "WEAP22", "AK", 500},            {30, 357, "WEAP31", "Sniper Rifle", 50},
            {31, 358, "WEAP32", "Laser Sniper", 50},   {32, 359, "WEAP29", "Rocket Launcher", 25},
            {33, 360, "WEAP30", "Flame-Thrower", 500}, {34, 361, "WEAP27", "M249", 500},
            {35, 362, "WEAP28", "Minigun", 1000},      {38, 364, "WEAP00", "Camera", 50},
            {39, 367, nullptr, "Binoculars", 1},
        };
        const WeaponDef *weaponDef(int type)
        {
            for (const WeaponDef &w : kWeapons)
                if (w.type == type)
                    return &w;
            return nullptr;
        }

        struct LoadoutDef
        {
            const char *name;
            std::array<int, 8> types; // 0 ends the list early
        };
        constexpr LoadoutDef kLoadouts[] = {
            {"Street", {6, 17, 19, 21, 24, 0}},
            {"Military", {9, 12, 20, 22, 29, 30, 32, 0}},
            {"Heavy", {11, 12, 20, 22, 27, 28, 31, 35}},
        };

        struct LandmarkDef
        {
            const char *zoneKey; // the district's name in the game's text
            const char *fallback;
            float x, y;          // a street-level spot in it (z: the ground there)
        };
        // Points chosen by teleporting to each and looking (docs/TRAINER.md); rooftops and water left out.
        constexpr LandmarkDef kLandmarks[] = {
            {"A_PORT", "Escobar International", -1446.2f, -820.2f}, {"ARMYB", "Fort Baxter Air Base", -1648.87f, -261.0f},
            {"BEACH1", "Ocean Beach", 83.8f, -1284.7f},            {"BEACH2", "Washington Beach", 571.0f, -640.8f},
            {"BEACH3", "Vice Point", 351.4f, 339.0f},              {"DTOWN", "Downtown", -834.4f, 1403.7f},
            {"HAITI", "Little Haiti", -958.2f, 85.6f},             {"HAVANA", "Little Havana", -978.2f, -570.1f},
            {"STARI", "Starfish Island", -426.4f, -529.9f},        {"PORNI", "Prawn Island", -60.4f, 948.9f},
            {"GOLFC", "Leaf Links", 49.1f, -99.4f},                {"DOCKS", "Viceport", -599.9f, -1517.0f},
            {"JUNKY", "Junkyard", -1302.5f, 93.7f},
        };

        constexpr const char *kWeatherNames[] = {"Sunny", "Cloudy", "Rainy", "Foggy", "Extra sunny", "Hurricane",
                                                 "Extra colours", "Ultra sunny"};

        // Vehicle models that cannot be spawned usefully (docs/TRAINER.md: every model was spawned in a test run).
        bool vehicleExcluded(int) { return false; }

        // ---- State ----------------------------------------------------------------------------------------------------
        std::array<std::atomic<bool>, ToggleCount> g_toggles{};
        std::atomic<bool> g_ready{false};
        std::atomic<bool> g_inVehicle{false};
        std::atomic<int> g_speed{100};
        std::atomic<int> g_clockHour{12};
        std::atomic<int> g_freeCameraSpeed{20};
        bool g_wasFlying = false; // game thread: the free camera was flying at the last frame kick

        struct Action
        {
            // game thread: fill `code` (or do the work itself and leave it empty) and set the notice; false refuses,
            // with the reason in the notice
            std::function<bool(uint8_t *ram, Scm &code, std::string &notice)> build;
            ps2control::Reply reply; // the control channel's, or empty
            std::vector<int> models; // loaded before build() runs
        };
        std::mutex g_actionsMutex;
        std::deque<Action> g_actions;

        struct Pending // game thread
        {
            uint32_t script = 0;
            Action action;
            std::string notice;
        };
        std::vector<Pending> g_pending;
        uint32_t g_tickScript = 0; // the held toggles' script of the last tick
        // An action waiting for its models: requested with REQUEST_MODEL, then polled each frame. (LOAD_ALL_MODELS_NOW
        // loads them inside the frame instead - and never returns when streaming memory is full: 28 different
        // vehicles parked in one place hung the game thread, 2026-10-26.)
        struct Loading
        {
            bool active = false, requested = false;
            Action action;
            uint64_t deadline = 0;
            uint32_t script = 0;
        };
        Loading g_loading;
        constexpr uint64_t kLoadFrames = 300; // ~10 s at 30 fps
        uint32_t g_lastGameTime = 0;
        uint64_t g_runFrames = 0;
        uint32_t g_protectedCar = 0;   // the vehicle VehicleInvincible last set its proofs on
        int g_frozenHour = 12, g_frozenMinute = 0;
        std::array<int, 40> g_clipSize{}; // NoReload: the fullest clip seen per weapon type

        std::mutex g_namesMutex;
        uint32_t g_namesFrom = 0; // the text table's entries the names were built from
        std::vector<Entry> g_vehicleNames, g_weaponNames, g_landmarkNames, g_storyNames, g_jobNames;
        std::vector<std::string> g_missionKeys; // per mission index: its text name (LOAD_MISSION_TEXT), "" if none
        std::atomic<bool> g_missionRunning{false};
        std::atomic<int> g_vehicleModel{0}; // the model the player drives (as of the last frame kick), 0 on foot

        // Last Stand (DIA_C5) launched from here (game-internals.md 11g). Its pass plays the final cutscene and leaves the
        // screen faded out for the credits - which the story starts only when every chapter is done. Out of story order
        // nothing fades it in again ("Loading..." over black for good), so the trainer rolls the game's credits itself.
        struct Finale // game thread
        {
            bool armed = false;    // the launch is queued
            bool started = false;  // the mission has run
            bool passed = false;   // its script was seen in the final cutscene
            uint32_t endTime = 0;  // game ms when it ended
            uint32_t ped = 0;
        };
        Finale g_finale;
        constexpr const char *kFinaleKey = "DIA_C5";
        constexpr uint32_t kCutsceneFrom = 0x00022F5Eu, kCutsceneTo = 0x00022F97u; // main script: load, play, clear it
        constexpr uint32_t kCredits = 0x00024DC3u;    // main script: the credits subroutine (its locals 0..42)
        constexpr uint32_t kCreditsReturn = 0x00016E90u; // main script: a TERMINATE_THIS_SCRIPT to return onto
        constexpr uint32_t kCreditsDelayMs = 5500u;   // the story waits 5 s (WAIT 5000) after the mission's end

        // The side jobs the launcher offers (their script names; the game's text names most of them).
        // Each runs only with the player in its vehicle ("You have 39 seconds to return to your vehicle"): the
        // launcher puts him in one first.
        struct JobDef
        {
            const char *key;
            const char *fallback;
            int vehicle; // model id
        };
        constexpr JobDef kJobs[] = {{"PARAMED", "Paramedic", 227 /* AMBULAN */}, {"VIG", "Vigilante", 207 /* POLICEM */},
                                    {"FIRETRK", "Fire Truck", 194 /* FIRETRK */}, {"CHOPLIF", "Air Rescue", 189 /* AIRAMB */},
                                    {"TAXI", "Taxi", 226 /* TAXI */}};

        // A story mission's text name: three letters, _, the chapter (A / B / C), a number - JER_A1, LAN_C10.
        bool storyKey(const std::string &k)
        {
            return k.size() >= 6 && k[3] == '_' && (k[4] == 'A' || k[4] == 'B' || k[4] == 'C') &&
                   std::all_of(k.begin() + 5, k.end(), [](char c) { return c >= '0' && c <= '9'; });
        }

        std::mutex g_noticesMutex;
        std::deque<Notice> g_notices;

        struct Saved
        {
            bool valid = false;
            float x = 0, y = 0, z = 0;
            uint32_t area = 0;
        };
        Saved g_saved; // game thread
        std::atomic<bool> g_hasSaved{false};

        std::string fmt(const char *format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        void post(const std::string &text)
        {
            std::lock_guard<std::mutex> lock(g_noticesMutex);
            g_notices.push_back({text, now()});
            while (g_notices.size() > 8)
                g_notices.pop_front();
        }

        void queue(std::function<bool(uint8_t *, Scm &, std::string &)> build, ps2control::Reply reply = {},
                   std::vector<int> models = {})
        {
            std::lock_guard<std::mutex> lock(g_actionsMutex);
            g_actions.push_back({std::move(build), std::move(reply), std::move(models)});
        }

        bool modelLoaded(const uint8_t *ram, int model)
        {
            const uint32_t streaming = rd32(ram, kStreaming);
            const uint32_t infos = streaming ? rd32(ram, streaming + 200) : 0;
            return infos && ram[(infos + model * 0x18u + 0xC) & kRamMask] == 1;
        }

        bool driving(const uint8_t *ram, uint32_t ped) { return rd32(ram, ped + kPedVehicle) && rd32(ram, ped + kPedState) == 0x37; }
        bool alive(const uint8_t *ram, uint32_t ped)
        {
            const uint32_t state = rd32(ram, ped + kPedState);
            return state != 0x39 && state != 0x3A;
        }

        // ---- Names from the game's text ---------------------------------------------------------------------------
        std::string keyAt(const uint8_t *ram, uint32_t a)
        {
            std::string key;
            for (uint32_t k = 0; k < 8; ++k)
            {
                const char c = static_cast<char>(ram[(a + k) & kRamMask]);
                if (!c)
                    break;
                key += c;
            }
            return key;
        }

        // UTF-16 text to UTF-8; the game's ~x~ formatting tokens dropped.
        std::string textAt(const uint8_t *ram, uint32_t a)
        {
            std::string out;
            bool token = false;
            for (int n = 0; n < 64; ++n, a += 2)
            {
                const uint16_t c = static_cast<uint16_t>(ram[a & kRamMask] | (ram[(a + 1) & kRamMask] << 8));
                if (!c)
                    break;
                if (c == '~')
                {
                    token = !token;
                    continue;
                }
                if (token)
                    continue;
                if (c < 0x80)
                    out += static_cast<char>(c);
                else if (c < 0x800)
                {
                    out += static_cast<char>(0xC0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xE0 | (c >> 12));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            return out;
        }

        // Game thread: the name lists, once the game's text is in memory (again if it is reloaded).
        void refreshNames(const uint8_t *ram)
        {
            const uint32_t table = rd32(ram, kTextTable);
            if (!table)
                return;
            const uint32_t entries = rd32(ram, table), count = rd32(ram, table + 4);
            static uint32_t lastTry = 0; // a second's frames between tries while the models are missing
            if (!entries || !count || count > 20000 || entries == g_namesFrom || g_runFrames - lastTry < 30)
                return;
            lastTry = static_cast<uint32_t>(g_runFrames);
            const uint32_t modelInfos = rd32(ram, kModelInfos);
            if (!modelInfos)
                return;
            std::unordered_map<std::string, std::string> text;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = entries + i * 12;
                text.emplace(keyAt(ram, e + 4), textAt(ram, rd32(ram, e)));
            }
            auto lookup = [&](const char *key, const std::string &fallback) {
                const auto it = key ? text.find(key) : text.end();
                return it != text.end() && !it->second.empty() ? it->second : fallback;
            };

            std::vector<Entry> vehicles;
            for (int id = kVehicleFirst; id <= kVehicleLast; ++id)
            {
                const uint32_t mi = rd32(ram, modelInfos + id * 4u);
                if (!mi || vehicleExcluded(id))
                    continue;
                const std::string key = keyAt(ram, mi + kModelGameName);
                if (key.empty())
                    continue;
                std::string fallback = key; // ADMIRAL -> Admiral (AEROPL, the airliner, has no text)
                std::transform(fallback.begin() + 1, fallback.end(), fallback.begin() + 1, [](char c) { return static_cast<char>(std::tolower(c)); });
                if (key == "AEROPL")
                    fallback = "Airliner";
                vehicles.push_back({lookup(key.c_str(), fallback), id});
            }
            std::sort(vehicles.begin(), vehicles.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
            for (size_t k = 0; k < vehicles.size(); ++k) // models sharing a name (police and taxi variants): numbered
            {
                size_t end = k + 1;
                while (end < vehicles.size() && vehicles[end].name == vehicles[k].name)
                    ++end;
                if (end - k > 1)
                    for (size_t n = k; n < end; ++n)
                        vehicles[n].name += fmt(" (%zu)", n - k + 1);
                k = end - 1;
            }

            std::vector<Entry> weapons;
            for (const WeaponDef &w : kWeapons)
                weapons.push_back({lookup(w.textKey, w.fallback), w.type});
            std::vector<Entry> landmarks;
            for (size_t k = 0; k < std::size(kLandmarks); ++k)
                landmarks.push_back({lookup(kLandmarks[k].zoneKey, kLandmarks[k].fallback), static_cast<int>(k)});
            std::sort(landmarks.begin(), landmarks.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });

            // Missions: the title's text key is the mission's text name with a two-digit number (JER_A1 -> JER_A01
            // "Soldier"); a few side jobs' titles are in quotes ('Paramedic').
            std::vector<Entry> story, jobs;
            auto title = [&](const std::string &key, const char *fallback) {
                std::string padded = key;
                if (storyKey(key) && key.size() == 6)
                    padded = key.substr(0, 5) + "0" + key.substr(5);
                std::string t = lookup(padded.c_str(), lookup(key.c_str(), fallback ? fallback : key));
                if (t.size() > 2 && t.front() == '\'' && t.back() == '\'')
                    t = t.substr(1, t.size() - 2);
                return t;
            };
            for (size_t i = 0; i < g_missionKeys.size(); ++i)
            {
                const std::string &key = g_missionKeys[i];
                if (storyKey(key))
                    story.push_back({title(key, nullptr), static_cast<int>(i)});
                for (const JobDef &j : kJobs)
                    if (key == j.key)
                        jobs.push_back({title(key, j.fallback), static_cast<int>(i)});
            }
            // story order is the table's, except two kept before the story block (44..): Soldier (JER_A1) opens the
            // story, In the Air Tonight (REN_C7, chapter C) ends it
            auto order = [](const Entry &e) { return e.id < 44 && g_missionKeys[e.id][4] == 'C' ? 1000 + e.id : e.id; };
            std::sort(story.begin(), story.end(), [&](const Entry &a, const Entry &b) { return order(a) < order(b); });

            std::lock_guard<std::mutex> lock(g_namesMutex);
            g_vehicleNames = std::move(vehicles);
            g_weaponNames = std::move(weapons);
            g_landmarkNames = std::move(landmarks);
            g_storyNames = std::move(story);
            g_jobNames = std::move(jobs);
            // the text is loaded before the vehicle models are registered: until they are, build the lists again
            if (!g_vehicleNames.empty())
                g_namesFrom = entries;
        }

        std::string nameOf(const std::vector<Entry> &list, int id, const std::string &fallback)
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            for (const Entry &e : list)
                if (e.id == id)
                    return e.name;
            return fallback;
        }

        // ---- The held toggles ----------------------------------------------------------------------------------------
        // Every 10 running frames: values written directly, and one script for the commands that must be repeated
        // (a mission, death or arrest can reset them).
        void tick(uint8_t *ram, uint32_t ped)
        {
            const bool on[ToggleCount] = {toggle(GodMode),         toggle(NeverWanted),     toggle(UnlimitedAmmo),
                                          toggle(NoReload),        toggle(InfiniteSprint),  toggle(FastReload),
                                          toggle(PoliceIgnore),    toggle(EveryoneIgnores), toggle(VehicleInvincible),
                                          toggle(WarpIntoSpawned), toggle(FreezeTime),     toggle(FreeCamera),
                                          toggle(HideHud)};
            const bool refresh = g_runFrames % 60 == 0; // the commands that stick: every 2 s at 30 fps
            Scm code;

            if (on[GodMode])
            {
                if (rdf(ram, ped + kPedHealth) < 100.0f)
                    wrf(ram, ped + kPedHealth, 100.0f);
                if (refresh)
                {
                    code.op(SET_CHAR_PROOFS).player().b(true).b(true).b(true).b(true).b(true);
                    code.op(SET_CHAR_DROWNS_IN_WATER).player().b(false);
                }
            }
            if (on[NeverWanted])
                code.op(CLEAR_WANTED_LEVEL).player();
            if (on[UnlimitedAmmo] || on[NoReload])
                for (int s = 0; s < kWeaponSlots; ++s)
                {
                    const uint32_t slot = ped + kPedWeapons + s * kWeaponSlotSize;
                    const uint32_t type = rd32(ram, slot);
                    if (type < 12 || type >= g_clipSize.size()) // unarmed and melee: nothing to count
                        continue;
                    const uint32_t clip = rd32(ram, slot + 8), total = rd32(ram, slot + 12);
                    g_clipSize[type] = std::max<int>(g_clipSize[type], static_cast<int>(clip));
                    if (on[UnlimitedAmmo] && total < 9000)
                        wr32(ram, slot + 12, 9999);
                    const uint32_t left = rd32(ram, slot + 12); // the total counts the clip
                    if (on[NoReload] && static_cast<int>(clip) < g_clipSize[type] && left > clip)
                        wr32(ram, slot + 8, std::min<uint32_t>(g_clipSize[type], left));
                }
            if (refresh)
            {
                if (on[InfiniteSprint])
                    code.op(SET_PLAYER_NEVER_GETS_TIRED).player().b(true);
                if (on[FastReload])
                    code.op(SET_PLAYER_FAST_RELOAD).player().b(true);
                if (on[PoliceIgnore])
                    code.op(SET_POLICE_IGNORE_PLAYER).player().b(true);
                if (on[EveryoneIgnores])
                    code.op(SET_EVERYONE_IGNORE_PLAYER).player().b(true);
            }
            if (on[VehicleInvincible] && driving(ram, ped))
            {
                const uint32_t car = rd32(ram, ped + kPedVehicle);
                if (rdf(ram, car + kCarHealth) < 1000.0f)
                    wrf(ram, car + kCarHealth, 1000.0f);
                if (car != g_protectedCar || refresh)
                {
                    code.storePlayerCar();
                    code.op(SET_CAR_PROOFS).car().b(true).b(true).b(true).b(true).b(true);
                    code.op(SET_CAR_CAN_BE_DAMAGED).car().b(false);
                    code.op(SET_UPSIDEDOWN_CAR_NOT_DAMAGED).car().b(true);
                    g_protectedCar = car;
                }
            }
            if (on[FreezeTime])
                code.op(SET_TIME_OF_DAY).i(g_frozenHour).i(g_frozenMinute);

            if (!code.empty())
            {
                const std::vector<uint8_t> bytes = code.finished();
                g_tickScript = vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), "trainer");
            }
        }

        // What turning a toggle on or off does at once.
        void applyToggle(Toggle t, bool on)
        {
            queue([t, on](uint8_t *ram, Scm &code, std::string &notice) {
                static const char *const names[ToggleCount] = {"God mode", "Never wanted", "Unlimited ammo", "No reload",
                                                               "Infinite sprint", "Fast reload", "Police ignore you",
                                                               "Everyone ignores you", "Vehicle invincibility",
                                                               "Get into spawned vehicles", "Freeze time", "Free camera",
                                                               "Hide HUD"};
                notice = std::string(names[t]) + (on ? " on" : " off");
                const uint32_t ped = rd32(ram, kPlayerPed);
                switch (t)
                {
                case GodMode:
                    code.op(SET_CHAR_PROOFS).player().b(on).b(on).b(on).b(on).b(on);
                    code.op(SET_CHAR_DROWNS_IN_WATER).player().b(!on);
                    if (on)
                        code.op(SET_CHAR_HEALTH).player().i(100);
                    break;
                case NeverWanted:
                    if (on)
                        code.op(CLEAR_WANTED_LEVEL).player();
                    break;
                case InfiniteSprint: code.op(SET_PLAYER_NEVER_GETS_TIRED).player().b(on); break;
                case FastReload: code.op(SET_PLAYER_FAST_RELOAD).player().b(on); break;
                case PoliceIgnore: code.op(SET_POLICE_IGNORE_PLAYER).player().b(on); break;
                case EveryoneIgnores: code.op(SET_EVERYONE_IGNORE_PLAYER).player().b(on); break;
                case VehicleInvincible:
                    g_protectedCar = 0;
                    if (driving(ram, ped)) // the tick protects the vehicle; turning it off gives it back its damage
                    {
                        code.storePlayerCar();
                        code.op(SET_CAR_PROOFS).car().b(on).b(on).b(on).b(on).b(on);
                        code.op(SET_CAR_CAN_BE_DAMAGED).car().b(!on);
                        code.op(SET_UPSIDEDOWN_CAR_NOT_DAMAGED).car().b(on);
                    }
                    break;
                case FreezeTime:
                    g_frozenHour = ram[kClockHours];
                    g_frozenMinute = ram[kClockHours + 1];
                    if (on)
                        notice += fmt(" at %02d:%02d", g_frozenHour, g_frozenMinute);
                    break;
                default: break;
                }
                return true;
            });
        }

        // ---- The game thread's queue ----------------------------------------------------------------------------
        void finish(Pending &p, bool ok, const std::string &why = {})
        {
            const std::string text = ok ? p.notice : why;
            if (!text.empty())
                post(text);
            if (p.action.reply)
                p.action.reply(ok ? "ok " + p.notice : "error " + why);
        }

        // In order, until one starts a script: the next waits until it has run (a frame), so an action sees what the
        // ones before it did and a control-channel answer queued last comes after all of them.
        void runActions(uint8_t *ram, uint32_t ped)
        {
            for (;;)
            {
                Action a;
                if (g_loading.active)
                {
                    if (!g_loading.requested) // (again, after a pause took the request back out)
                    {
                        Scm request;
                        for (int m : g_loading.action.models)
                            request.op(REQUEST_MODEL).i(m);
                        const std::vector<uint8_t> bytes = request.finished();
                        g_loading.script = vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), "trainer");
                        g_loading.requested = g_loading.script != 0;
                        return;
                    }
                    const bool loaded = std::all_of(g_loading.action.models.begin(), g_loading.action.models.end(),
                                                    [ram](int m) { return modelLoaded(ram, m); });
                    if (!loaded && g_runFrames < g_loading.deadline)
                        return;
                    a = std::move(g_loading.action);
                    g_loading.active = false;
                    if (!loaded)
                    {
                        Scm release;
                        for (int m : a.models)
                            release.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(m);
                        const std::vector<uint8_t> bytes = release.finished();
                        vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), "trainer");
                        Pending p{0, std::move(a), {}};
                        finish(p, false, "Could not load it: the game's memory for models is full here (leave the area or "
                                         "lose some spawned vehicles first)");
                        return;
                    }
                    a.models.clear();
                }
                else
                {
                    std::lock_guard<std::mutex> lock(g_actionsMutex);
                    if (g_actions.empty())
                        return;
                    a = std::move(g_actions.front());
                    g_actions.pop_front();
                }
                if (!a.models.empty())
                {
                    g_loading = {true, false, std::move(a), g_runFrames + kLoadFrames, 0};
                    continue;
                }
                Pending p{0, std::move(a), {}};
                Scm code;
                std::string notice;
                if (!alive(ram, ped))
                {
                    finish(p, false, "Not now: the player is down");
                    continue;
                }
                if (!p.action.build(ram, code, notice))
                {
                    finish(p, false, notice);
                    continue;
                }
                p.notice = notice;
                if (code.empty())
                {
                    finish(p, true);
                    continue;
                }
                const std::vector<uint8_t> bytes = code.finished();
                p.script = vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), "trainer");
                if (!p.script)
                {
                    finish(p, false, bytes.size() > 176 ? "Internal error: script too long" : "The game's script slots are all in use");
                    continue;
                }
                g_pending.push_back(std::move(p));
                return;
            }
        }

        // Every frame while a launched Last Stand is armed (g_finale). The pass is the final cutscene: the mission script
        // in the main script's cutscene subroutine (it calls no other). The story's own end (main script 0x2F9C9, after
        // each mission) waits 5 s and calls the credits when chapter $G82 is 2, $G296 (credits seen) is 0 and the
        // chapter flags $G16 $G19 $G1A $G1B $G1D are 1; then it is left to the story.
        void watchFinale(uint8_t *ram, uint32_t ped, uint32_t time, bool running)
        {
            if (!g_finale.armed)
                return;
            if (ram[kMissionRunning])
            {
                g_finale.started = true;
                for (uint32_t s = rd32(ram, kActiveScripts), n = 0; s && n < 256 && !g_finale.passed; s = rd32(ram, s), ++n)
                {
                    const uint32_t ip = rd32(ram, s + 0x10);
                    g_finale.passed = ip >= kCutsceneFrom && ip <= kCutsceneTo;
                }
                return;
            }
            if (!g_finale.started)
                return;
            if (!g_finale.passed) // failed, or a game was loaded
            {
                g_finale = {};
                return;
            }
            if (!g_finale.endTime)
            {
                g_finale.endTime = time;
                g_finale.ped = ped;
                return;
            }
            if (time < g_finale.endTime || ped != g_finale.ped || !alive(ram, ped)) // a load, a new game
            {
                g_finale = {};
                return;
            }
            if (!running || time - g_finale.endTime < kCreditsDelayMs)
                return;
            g_finale = {};
            const uint32_t space = rd32(ram, kScriptSpacePtr);
            const auto global = [&](uint32_t n) { return static_cast<int32_t>(rd32(ram, space + 4 * n)); };
            if (global(0x82) == 2 && global(0x296) == 0 && global(0x16) == 1 && global(0x19) == 1 && global(0x1A) == 1 &&
                global(0x1B) == 1 && global(0x1D) == 1)
                return;
            // a script of its own at the credits, returning onto a TERMINATE_THIS_SCRIPT of the main script; both checked
            const auto opcodeAt = [&](uint32_t ip) { return static_cast<uint16_t>(rd32(ram, space + ip) & 0xFFFF); };
            if (opcodeAt(kCredits) != 0x0004 || opcodeAt(kCreditsReturn) != TERMINATE)
                return post("Last Stand out of story order: the credits are not where expected");
            if (vcs::rig::startScriptAt(ram, kCredits, "credits", kCreditsReturn))
                post("Last Stand out of story order: the game's credits");
        }

        std::vector<std::string> words(const std::string &args)
        {
            std::istringstream in(args);
            std::vector<std::string> out;
            for (std::string w; in >> w;)
                out.push_back(w);
            return out;
        }

        std::string listReply(const std::vector<Entry> &list)
        {
            std::string out = "ok";
            for (const Entry &e : list)
                out += fmt(" %d:", e.id) + e.name + ";";
            return out;
        }

        // Control channel: trainer <command> [arg]. Actions answer once the game has run them.
        void command(const std::string &args, ps2control::Reply reply)
        {
            static const std::pair<const char *, Toggle> kToggleNames[] = {
                {"god", GodMode},          {"wanted", NeverWanted},          {"ammo", UnlimitedAmmo},
                {"reload", NoReload},      {"sprint", InfiniteSprint},       {"fastreload", FastReload},
                {"police", PoliceIgnore},  {"ignore", EveryoneIgnores},      {"vehicle", VehicleInvincible},
                {"warp", WarpIntoSpawned}, {"freeze", FreezeTime},        {"freecam", FreeCamera},
                {"hidehud", HideHud},
            };
            const auto w = words(args);
            const std::string verb = w.empty() ? "" : w[0];
            const int arg = w.size() > 1 ? std::atoi(w[1].c_str()) : 0;
            // answers once everything queued before has run
            // answers once everything queued before has run, with the newest notice (what was done, or why not)
            auto answerWhenDone = [&reply] {
                queue([](uint8_t *, Scm &, std::string &) { return true; }, [reply](const std::string &) {
                    const std::vector<Notice> n = notices(5.0);
                    reply("ok " + (n.empty() ? std::string() : n.back().text));
                });
            };
            if (verb == "state")
            {
                std::string out = fmt("ok ready=%d vehicle=%d speed=%d mission=%d", ready() ? 1 : 0, inVehicle() ? 1 : 0,
                                      gameSpeed(), missionRunning() ? 1 : 0);
                for (const auto &[name, t] : kToggleNames)
                    out += fmt(" %s=%d", name, toggle(t) ? 1 : 0);
                return reply(out);
            }
            if (verb == "toggle" && w.size() >= 3)
            {
                for (const auto &[name, t] : kToggleNames)
                    if (w[1] == name)
                    {
                        setToggle(t, w[2] == "1" || w[2] == "on");
                        return answerWhenDone();
                    }
                return reply("error unknown toggle");
            }
            if (verb == "list" && w.size() >= 2)
            {
                if (w[1] == "vehicles")
                    return reply(listReply(vehicles()));
                if (w[1] == "weapons")
                    return reply(listReply(weapons()));
                if (w[1] == "loadouts")
                    return reply(listReply(loadouts()));
                if (w[1] == "landmarks")
                    return reply(listReply(landmarks()));
                if (w[1] == "weathers")
                    return reply(listReply(weathers()));
                if (w[1] == "story")
                    return reply(listReply(missions(true)));
                if (w[1] == "jobs")
                    return reply(listReply(missions(false)));
                return reply("error usage: trainer list vehicles|weapons|loadouts|landmarks|weathers|story|jobs");
            }
            if (verb == "freecam") // the free camera as of its last update
            {
                const freecam::Pose p = freecam::pose();
                return reply(fmt("ok active=%d x=%.2f y=%.2f z=%.2f yaw=%.4f pitch=%.4f fov=%.1f speed=%d hud=%u",
                                 p.active ? 1 : 0, p.x, p.y, p.z, p.yaw, p.pitch, p.fov, freeCameraSpeed(),
                                 vcs::hudDrawCount()));
            }
            if (verb == "notices")
            {
                std::string out = "ok";
                for (const Notice &n : notices(1e9))
                    out += " " + n.text + ";";
                return reply(out);
            }
            if (!ready())
                return reply("error not in game");
            // the actions, through the same functions the menu calls
            using Fn = std::function<void()>;
            const std::pair<const char *, Fn> kActions[] = {
                {"heal", [] { heal(); }},
                {"money", [arg] { addMoney(arg); }},
                {"setwanted", [arg] { setWanted(arg); }},
                {"weapon", [arg] { giveWeapon(arg); }},
                {"loadout", [arg] { giveLoadout(arg); }},
                {"noweapons", [] { removeWeapons(); }},
                {"spawn", [arg] { spawnVehicle(arg); }},
                {"repair", [] { repairVehicle(); }},
                {"flip", [] { flipVehicle(); }},
                {"boost", [] { boostVehicle(); }},
                {"paint", [] { paintVehicle(); }},
                {"teleport", [arg] { teleport(arg); }},
                {"savepos", [] { savePosition(); }},
                {"returnpos", [] { returnToSaved(); }},
                {"camtp", [] { teleportToCamera(); }},
                {"camspeed", [arg] { setFreeCameraSpeed(arg); }},
                {"time", [arg] { setTime(arg); }},
                {"weather", [arg] { setWeather(arg); }},
                {"speed", [arg] { setGameSpeed(arg); }},
                {"mission", [arg] { launchMission(arg); }},
            };
            for (const auto &[name, fn] : kActions)
                if (verb == name)
                {
                    fn();
                    return answerWhenDone();
                }
            reply("error usage: trainer state | toggle <name> 0|1 | list <what> | notices | heal | money N | setwanted N | "
                  "weapon T | loadout N | noweapons | spawn MODEL | repair | flip | boost | paint | teleport N | savepos | "
                  "returnpos | camtp | camspeed M | freecam | time H | weather W | speed PERCENT | mission N");
        }
    }

    double now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    void install(PS2Runtime &)
    {
        g_toggles[WarpIntoSpawned] = true; // a spawned vehicle is to be driven
        ps2control::addCommand("trainer", command);
    }

    void onGameFrame(uint8_t *ram)
    {
        const uint32_t time = rd32(ram, kGameTimeMs);
        const bool running = time != g_lastGameTime;
        g_lastGameTime = time;
        const uint32_t ped = rd32(ram, kPlayerPed);
        const bool inGame = rd32(ram, kScriptSpacePtr) != 0 && ped != 0;
        g_ready = inGame;
        g_inVehicle = inGame && driving(ram, ped);
        g_clockHour = ram[kClockHours];
        g_missionRunning = inGame && ram[kMissionRunning] != 0;
        uint16_t model = 0;
        if (g_inVehicle)
            std::memcpy(&model, ram + ((rd32(ram, ped + kPedVehicle) + 0x56) & kRamMask), 2); // entity +0x56: model
        g_vehicleModel = model;
        refreshNames(ram);
        if (!inGame)
            g_toggles[FreeCamera] = false; // (a game loading, the title screen): the camera is the game's again

        // Scripts finish in the frame they start. One still waiting means the game paused before running it (its
        // menu, the save screen): take it back out so it cannot be saved, and run it once the game runs again.
        for (auto it = g_pending.begin(); it != g_pending.end();)
        {
            if (!vcs::rig::scriptActive(ram, it->script))
            {
                finish(*it, true);
                it = g_pending.erase(it);
            }
            else if (!running)
            {
                vcs::rig::cancelCodeScript(ram, it->script);
                std::lock_guard<std::mutex> lock(g_actionsMutex);
                g_actions.push_front(std::move(it->action));
                it = g_pending.erase(it);
            }
            else
                ++it;
        }
        if (vcs::rig::scriptActive(ram, g_tickScript) && !running)
            vcs::rig::cancelCodeScript(ram, g_tickScript);
        if (g_loading.active && vcs::rig::scriptActive(ram, g_loading.script) && !running)
        {
            vcs::rig::cancelCodeScript(ram, g_loading.script);
            g_loading.requested = false;
        }
        if (inGame)
            watchFinale(ram, ped, time, running);
        if (!running || !inGame || !g_pending.empty())
            return;
        ++g_runFrames;
        // the free camera ended: the game's camera back behind the player at once (RESTORE_CAMERA_JUMPCUT; not while a
        // mission's script camera holds it - that one carries on where it was)
        const bool flying = freecam::flying();
        if (g_wasFlying && !flying && rd32(ram, kCameraLooksAtPlayer) == 1)
        {
            Scm code;
            code.op(RESTORE_CAMERA_JUMPCUT);
            const std::vector<uint8_t> bytes = code.finished();
            vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), "trainer");
        }
        g_wasFlying = flying;
        runActions(ram, ped);
        if (g_runFrames % 10 == 0 && alive(ram, ped) && !vcs::rig::scriptActive(ram, g_tickScript))
            tick(ram, ped);
    }

    bool toggle(Toggle t) { return t >= 0 && t < ToggleCount && g_toggles[t].load(std::memory_order_relaxed); }

    void setToggle(Toggle t, bool on)
    {
        if (t < 0 || t >= ToggleCount || g_toggles[t].exchange(on) == on)
            return;
        applyToggle(t, on);
    }

    bool ready() { return g_ready.load(std::memory_order_relaxed); }
    bool inVehicle() { return g_inVehicle.load(std::memory_order_relaxed); }

    void heal()
    {
        queue([](uint8_t *, Scm &code, std::string &notice) {
            code.op(SET_CHAR_HEALTH).player().i(100);
            code.op(ADD_ARMOUR_TO_CHAR).player().i(100);
            notice = "Health and armour filled";
            return true;
        });
    }

    void addMoney(int amount)
    {
        queue([amount](uint8_t *, Scm &code, std::string &notice) {
            code.op(ADD_SCORE).player().i(amount);
            notice = fmt("$%d added", amount);
            return true;
        });
    }

    void setWanted(int stars)
    {
        stars = std::clamp(stars, 0, 6);
        queue([stars](uint8_t *, Scm &code, std::string &notice) {
            if (stars > 0 && toggle(NeverWanted))
                return (notice = "Never wanted is on", false);
            code.op(CLEAR_WANTED_LEVEL).player();
            if (stars > 0)
                code.op(ALTER_WANTED_LEVEL_NO_DROP).player().i(stars);
            notice = stars ? fmt("Wanted level %d", stars) : "Wanted level cleared";
            return true;
        });
    }

    // The model is loaded first (queue's models): a weapon given without its model loaded is held invisible. The
    // script's own REQUEST_MODEL / MARK_MODEL_AS_NO_LONGER_NEEDED pair hands it back to the streaming afterwards.
    static void spawnVehicleAs(int model, bool getIn);

    static void addWeapon(Scm &code, const WeaponDef &w, bool select)
    {
        code.op(REQUEST_MODEL).i(w.model);
        code.op(GIVE_WEAPON_TO_CHAR).player().i(w.type).i(w.ammo);
        if (select)
            code.op(SET_CURRENT_CHAR_WEAPON).player().i(w.type);
        code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(w.model);
    }

    void giveWeapon(int type)
    {
        const WeaponDef *w = weaponDef(type);
        if (!w)
            return post("No such weapon");
        const std::string name = nameOf(g_weaponNames, type, w->fallback);
        queue([w, name](uint8_t *, Scm &code, std::string &notice) {
            if (toggle(NoReload) || toggle(UnlimitedAmmo))
                g_clipSize[w->type] = 0; // learn the clip again (a new weapon comes with a full one)
            addWeapon(code, *w, true);
            notice = name + " given";
            return true;
        }, {}, {w->model});
    }

    void giveLoadout(int which)
    {
        if (which < 0 || which >= static_cast<int>(std::size(kLoadouts)))
            return post("No such loadout");
        const LoadoutDef &l = kLoadouts[which];
        // one script per weapon: a whole loadout is longer than a script's code space
        for (size_t k = 0; k < l.types.size() && l.types[k]; ++k)
            if (const WeaponDef *w = weaponDef(l.types[k]))
                queue([w](uint8_t *, Scm &code, std::string &) { return (addWeapon(code, *w, false), true); }, {}, {w->model});
        queue([&l](uint8_t *, Scm &, std::string &notice) { return (notice = std::string(l.name) + " loadout given", true); });
    }

    void removeWeapons()
    {
        queue([](uint8_t *, Scm &code, std::string &notice) {
            code.op(REMOVE_ALL_CHAR_WEAPONS).player();
            notice = "Weapons removed";
            return true;
        });
    }

    void spawnVehicle(int model) { spawnVehicleAs(model, false); }

    static void spawnVehicleAs(int model, bool getIn)
    {
        if (model < kVehicleFirst || model > kVehicleLast)
            return post("No such vehicle");
        const std::string name = nameOf(g_vehicleNames, model, fmt("Vehicle %d", model));
        queue([model, name, getIn](uint8_t *ram, Scm &code, std::string &notice) {
            const uint32_t ped = rd32(ram, kPlayerPed);
            const bool inCar = driving(ram, ped);
            const bool warp = getIn || toggle(WarpIntoSpawned);
            // where: ahead of the player (of the vehicle when driving; the ped's own matrix is not its position then),
            // facing the same way. Locals 1..3 position, 4 heading, 0 the new vehicle.
            if (inCar)
            {
                code.storePlayerCar();
                code.op(GET_OFFSET_FROM_CAR_IN_WORLD_COORDS).car().f(0.0f).f(8.0f).f(0.5f).local(1).local(2).local(3);
                code.op(GET_CAR_HEADING).car().local(4);
            }
            else
            {
                code.op(GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS).player().f(0.0f).f(5.0f).f(0.0f).local(1).local(2).local(3);
                code.op(GET_CHAR_HEADING).player().local(4);
            }
            code.op(REQUEST_MODEL).i(model);
            code.op(CREATE_CAR).i(model).local(1).local(2).local(3).local(0);
            code.op(SET_CAR_HEADING).local(0).local(4);
            if (warp)
            {
                if (inCar)
                    code.op(WARP_CHAR_FROM_CAR_TO_COORD).player().local(1).local(2).local(3);
                code.op(WARP_CHAR_INTO_CAR).player().local(0);
                // From one vehicle straight into another the camera keeps following the old one (the new target is
                // set, the active camera's is not). The game's RestoreWithJumpCut hands it the new one
                // (game-internals.md 12) - unless a mission's script camera holds it.
                if (inCar && rd32(ram, kCameraLooksAtPlayer) != 0)
                    code.op(RESTORE_CAMERA_JUMPCUT);
            }
            // handed back to the game: it stays while it is near or used, like any car
            code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(model);
            code.op(MARK_CAR_AS_NO_LONGER_NEEDED).local(0);
            g_protectedCar = 0;
            notice = name + " spawned";
            return true;
        }, {}, {model});
    }

    // The vehicle commands need the player in one: a vehicle command on no vehicle stops the script for good.
    static void vehicleAction(const char *done, std::function<void(Scm &)> body)
    {
        queue([done, body](uint8_t *ram, Scm &code, std::string &notice) {
            if (!driving(ram, rd32(ram, kPlayerPed)))
                return (notice = "Not in a vehicle", false);
            code.storePlayerCar();
            body(code);
            notice = done;
            return true;
        });
    }

    // Health back to full: the engine and fire. The dents stay - no script command repairs them (a Pay 'n' Spray does).
    void repairVehicle()
    {
        vehicleAction("Vehicle repaired", [](Scm &code) { code.op(SET_CAR_HEALTH).car().i(1000); });
    }

    // SET_CAR_HEADING rebuilds the matrix upright around its heading; SET_CAR_COORDINATES lifts it onto its wheels.
    void flipVehicle()
    {
        vehicleAction("Vehicle flipped upright", [](Scm &code) {
            code.op(GET_CAR_COORDINATES).car().local(1).local(2).local(3);
            code.op(GET_CAR_HEADING).car().local(4);
            code.op(SET_CAR_COORDINATES).car().local(1).local(2).local(3);
            code.op(SET_CAR_HEADING).car().local(4);
        });
    }

    void boostVehicle()
    {
        vehicleAction("Boost!", [](Scm &code) { code.op(SET_CAR_FORWARD_SPEED).car().f(45.0f); });
    }

    void paintVehicle()
    {
        static std::mt19937 rng{std::random_device{}()};
        const int a = static_cast<int>(rng() % 95), b = static_cast<int>(rng() % 95); // the 95 colours of carcols.dat
        vehicleAction("Vehicle painted", [a, b](Scm &code) { code.op(CHANGE_CAR_COLOUR).car().i(a).i(b); });
    }

    // Outdoors (the area first: drawn with an interior's area the city is black), the scene loaded, then onto the
    // ground (z -100). SET_CHAR_COORDINATES moves the vehicle when the player drives one.
    static void moveTo(Scm &code, uint32_t area, float x, float y, float z)
    {
        code.op(SET_AREA_VISIBLE).i(static_cast<int32_t>(area));
        code.op(LOAD_SCENE).f(x).f(y).f(z < -99.0f ? 20.0f : z);
        code.op(SET_CHAR_COORDINATES).player().f(x).f(y).f(z);
    }

    void teleport(int landmark)
    {
        if (landmark < 0 || landmark >= static_cast<int>(std::size(kLandmarks)))
            return post("No such place");
        const LandmarkDef &l = kLandmarks[landmark];
        const std::string name = nameOf(g_landmarkNames, landmark, l.fallback);
        queue([&l, name](uint8_t *, Scm &code, std::string &notice) {
            moveTo(code, 0, l.x, l.y, -100.0f);
            notice = "Teleported to " + name;
            return true;
        });
    }

    void savePosition()
    {
        queue([](uint8_t *ram, Scm &, std::string &notice) {
            const uint32_t ped = rd32(ram, kPlayerPed);
            const uint32_t at = driving(ram, ped) ? rd32(ram, ped + kPedVehicle) : ped;
            g_saved = {true, rdf(ram, at + kPedPosition), rdf(ram, at + kPedPosition + 4), rdf(ram, at + kPedPosition + 8),
                       rd32(ram, kCurrArea)};
            g_hasSaved = true;
            notice = fmt("Position saved (%.0f, %.0f, %.0f)", g_saved.x, g_saved.y, g_saved.z);
            return true;
        });
    }

    void returnToSaved()
    {
        queue([](uint8_t *, Scm &code, std::string &notice) {
            if (!g_saved.valid)
                return (notice = "No position saved", false);
            moveTo(code, g_saved.area, g_saved.x, g_saved.y, g_saved.z);
            notice = "Back at the saved position";
            return true;
        });
    }

    bool hasSavedPosition() { return g_hasSaved.load(std::memory_order_relaxed); }

    void teleportToCamera()
    {
        queue([](uint8_t *ram, Scm &code, std::string &notice) {
            const freecam::Pose p = freecam::pose();
            if (!freecam::flying() || !p.active)
                return (notice = "Turn the free camera on first", false);
            moveTo(code, rd32(ram, kCurrArea), p.x, p.y, -100.0f); // on the ground below it, in the area shown
            g_toggles[FreeCamera] = false;
            notice = fmt("Teleported below the camera (%.0f, %.0f)", p.x, p.y);
            return true;
        });
    }

    int freeCameraSpeed() { return g_freeCameraSpeed.load(std::memory_order_relaxed); }
    void setFreeCameraSpeed(int metresPerSecond) { g_freeCameraSpeed = std::clamp(metresPerSecond, 1, 200); }

    void setTime(int hour)
    {
        hour = ((hour % 24) + 24) % 24;
        queue([hour](uint8_t *, Scm &code, std::string &notice) {
            code.op(SET_TIME_OF_DAY).i(hour).i(0);
            g_frozenHour = hour;
            g_frozenMinute = 0;
            notice = fmt("Time %02d:00", hour);
            return true;
        });
    }

    void setWeather(int weather)
    {
        if (weather >= static_cast<int>(std::size(kWeatherNames)))
            return post("No such weather");
        queue([weather](uint8_t *, Scm &code, std::string &notice) {
            if (weather < 0)
            {
                code.op(RELEASE_WEATHER);
                notice = "Weather changes normally";
            }
            else
            {
                code.op(FORCE_WEATHER_NOW).i(weather);
                notice = std::string("Weather: ") + kWeatherNames[weather];
            }
            return true;
        });
    }

    void setGameSpeed(int percent)
    {
        percent = std::clamp(percent, 25, 200);
        g_speed = percent;
        queue([percent](uint8_t *, Scm &code, std::string &notice) {
            code.op(SET_TIME_SCALE).f(percent / 100.0f);
            notice = fmt("Game speed %d%%", percent);
            return true;
        });
    }

    int gameSpeed() { return g_speed.load(std::memory_order_relaxed); }
    int clockHour() { return g_clockHour.load(std::memory_order_relaxed); }

    std::vector<Entry> vehicles()
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        return g_vehicleNames;
    }

    std::vector<Entry> weapons()
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        if (!g_weaponNames.empty())
            return g_weaponNames;
        std::vector<Entry> out;
        for (const WeaponDef &w : kWeapons)
            out.push_back({w.fallback, w.type});
        return out;
    }

    std::vector<Entry> loadouts()
    {
        std::vector<Entry> out;
        for (size_t k = 0; k < std::size(kLoadouts); ++k)
            out.push_back({kLoadouts[k].name, static_cast<int>(k)});
        return out;
    }

    std::vector<Entry> landmarks()
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        if (!g_landmarkNames.empty())
            return g_landmarkNames;
        std::vector<Entry> out;
        for (size_t k = 0; k < std::size(kLandmarks); ++k)
            out.push_back({kLandmarks[k].fallback, static_cast<int>(k)});
        return out;
    }

    void setMissionScript(const std::vector<uint8_t> &d)
    {
        // the mission table (game-internals.md 11a): u32 count at file 0x767C, then the missions' start IPs (file
        // offset = IP + 8). Each mission opens with LOAD_MISSION_TEXT "<name>" (0x033A) or SCRIPT_NAME (0x0238).
        auto u32 = [&d](size_t at) {
            uint32_t v = 0;
            if (at + 4 <= d.size())
                std::memcpy(&v, d.data() + at, 4);
            return v;
        };
        const uint32_t count = u32(0x767C);
        if (count == 0 || count > 200)
            return;
        std::vector<std::string> keys(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t start = static_cast<size_t>(u32(0x7680 + 4 * i)) + 8;
            const size_t end = i + 1 < count ? static_cast<size_t>(u32(0x7680 + 4 * (i + 1))) + 8 : d.size();
            for (size_t p = start; p + 4 < std::min({d.size(), end, start + 64}); ++p)
            {
                const bool missionText = d[p] == 0x3A && d[p + 1] == 0x03, scriptName = d[p] == 0x38 && d[p + 1] == 0x02;
                if ((!missionText && !scriptName) || d[p + 2] != 0x0A)
                    continue;
                std::string name;
                for (size_t q = p + 3; q < d.size() && d[q] && name.size() < 15; ++q)
                    name += static_cast<char>(d[q]);
                keys[i] = name;
                if (missionText) // the text name wins over the script's own name
                    break;
            }
        }
        g_missionKeys = std::move(keys);
    }

    bool missionRunning() { return g_missionRunning.load(std::memory_order_relaxed); }

    // LOAD_AND_LAUNCH_MISSION_INTERNAL: what the game's own mission markers run. The mission starts where the player
    // is; it sets the scene up itself (most open with their cutscene).
    void launchMission(int index)
    {
        const std::string name = nameOf(g_storyNames, index, nameOf(g_jobNames, index, fmt("Mission %d", index)));
        int vehicle = 0;
        for (const JobDef &j : kJobs)
            if (index < static_cast<int>(g_missionKeys.size()) && g_missionKeys[index] == j.key)
                vehicle = j.vehicle;
        if (vehicle && !missionRunning() && g_vehicleModel != vehicle) // not in the job's vehicle: one to do it in
            spawnVehicleAs(vehicle, true);
        const bool finale = index < static_cast<int>(g_missionKeys.size()) && g_missionKeys[index] == kFinaleKey;
        queue([index, name, vehicle, finale](uint8_t *ram, Scm &code, std::string &notice) {
            if (ram[kMissionRunning])
                return (notice = "A mission is running: finish or fail it first", false);
            if (vehicle && !driving(ram, rd32(ram, kPlayerPed)))
                return (notice = "Could not get into the job's vehicle", false);
            if (finale)
                g_finale = {true};
            code.op(LOAD_AND_LAUNCH_MISSION_INTERNAL).i(index);
            notice = "Mission: " + name;
            if (g_toggles[FreeCamera].exchange(false)) // the mission's start is to be watched, not flown past
                notice += " (free camera off)";
            return true;
        });
    }

    std::vector<Entry> missions(bool story)
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        return story ? g_storyNames : g_jobNames;
    }

    std::vector<Entry> weathers()
    {
        std::vector<Entry> out{{"Normal (changes)", -1}};
        for (size_t k = 0; k < std::size(kWeatherNames); ++k)
            out.push_back({kWeatherNames[k], static_cast<int>(k)});
        return out;
    }

    std::vector<Notice> notices(double maxAgeSeconds)
    {
        const double t = now();
        std::lock_guard<std::mutex> lock(g_noticesMutex);
        std::vector<Notice> out;
        for (const Notice &n : g_notices)
            if (t - n.time <= maxAgeSeconds)
                out.push_back(n);
        return out;
    }

    void notifyScreenshot(const std::string &text)
    {
        std::lock_guard<std::mutex> lock(g_noticesMutex);
        g_notices.push_back({text, now(), true});
        while (g_notices.size() > 8)
            g_notices.pop_front();
    }
}
