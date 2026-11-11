// HD world further (FORK.md T2, docs/game-internals.md 9f / 9h / 9j): the detailed city drawn further than the game's
// own window, through the game's own world draw.
//
// What the game does (read from FUN_0023C188 / FUN_0023C820 / FUN_0023D740 / FUN_0023CED0 / FUN_0023E748 /
// FUN_00240ED8): the level keeps three cell slots (level + 0x248 current, + 0x250 previous / next, + 0x258 loading) and
// three area slots (+ 0x24C / 0x254 / 0x25C). A cell slot holds the cell's pieces (+ 0x1C count, + 0x20 the pieces;
// the first is the cell's sector list); an area slot holds the piece of a 3 x 3 area. Every piece starts with a pair
// list ({gid, geometry}) the game enters into the level's geometry table (**(level + 0x1C), 0xC per gid: geometry, its
// cached packet, owner). The world draw walks the current cell's list (eight buckets of 0x50-byte sector records) and
// draws each record whose table entry has geometry; the list holds the cell's detailed sectors (their geometry in the
// cell's and its area's pieces, ~260 m around it) and the low-detail sectors of the rest of the map, minus those the
// cell's detail replaces. A cell's list never names another cell's geometry (measured over the whole map,
// build/analysis/hd_world), so more detail needs more lists.
//
// What this does, with world.hd_radius > 0: the cells around the camera (not the current one) are read from the disc
// into the port's memory (vcs::memory::kHdWorldBase) with their area and conditional pieces, relocated as the game does
// (FUN_00324AC0) and their pairs entered into the table - only where the entry is empty, so the game's own entries are
// never touched. At the world draw the current slot's list is replaced by one built here: the game's records, the extra
// cells' detailed records (moved from their cell's centre to the current one's), and the current list's low-detail
// sectors that an extra cell's detail replaces marked hidden (0x8000, as the game marks them). During the cross-fade
// the previous slot gets the same. The game's own list is put back before any streaming code runs (the hooks below:
// read watch on the list pointer, only FUN_0023Cxxx-FUN_002413xx read it), so its streaming never sees ours.
//
// world.hd_radius = 0 installs nothing and leaves the game's path untouched (the reference).
#include "game/vcs_world_hd.h"

#include "disc/iso_image.h"
#include "game/vcs_memory.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vcs::world_hd
{
    namespace
    {
        constexpr uint32_t kLevelPtr = 0x0048F4F8u;
        constexpr uint32_t kHour = 0x004CD148u;      // the clock's hour (byte), FUN_002287E8's
        constexpr uint32_t kCameraPos = 0x006F4500u; // TheCamera's matrix position (0x6F44D0 + 0x30)
        constexpr uint32_t kFileTable = 0x005058A8u; // CdStream's open files: 0x14 each, +4 the first sector
        constexpr uint32_t kClassTable = 0x006EE048u, kGlobals = 0x006EE0C8u; // FUN_00324AC0's fixups

        constexpr uint32_t kChooseArea = 0x0023C820u;      // every frame, CGame::Process: picks / swaps the areas
        constexpr uint32_t kAreaForPosition = 0x0023C188u; // loads and teleports
        constexpr uint32_t kSwapArea = 0x0023D740u;
        constexpr uint32_t kDropPrevious = 0x0023CED0u;
        constexpr uint32_t kFlush = 0x002418F0u;
        // FUN_0023BAF0(level, id, ...): the level's data (east 1 / west 2) read again into the same level object and the
        // same geometry table - the bridge between the islands, a load (FUN_0023C188 calls it when level + 0x18 differs
        // from 0x4CD118)
        constexpr uint32_t kLevelLoad = 0x0023BAF0u;
        constexpr uint32_t kLevelId = 0x004CD118u;
        constexpr uint32_t kWorldDraw = 0x0023E748u; // FUN_0023E748(level, pass)

        constexpr uint32_t kDlrw = 0x57524C44u; // 'DLRW': a cell / conditional piece's index entry
        constexpr uint32_t kRecord = 0x50u;
        constexpr uint32_t kOwnerTag = 0x20000000u; // ours | y << 16 | x (the game's: 0x40000000 cells, 0x80000000 areas)
        constexpr uint32_t kMaxGid = 0x8000u;
        // sector ids: the detailed city below 3500, the low-detail sectors from 5500 (none between; whatever piece their
        // geometry is in - measured over every cell list on the disc, build/analysis/hd_world)
        constexpr uint16_t kLodIds = 4000u;
        constexpr uint32_t kSynthBytes = 0x80000u; // one built list: header + up to ~6500 records
        constexpr int kSynthBuffers = 3;            // per slot: a list is rewritten two builds after it was drawn
        constexpr uint64_t kFreeDelay = 8;          // frames before freed memory is reused (the GS may still read it)

        // the setting's rings: how far from the camera extra cells are kept
        constexpr float kReach[] = {0.0f, 190.0f, 320.0f, 460.0f};
        constexpr float kKeepMargin = 110.0f;

        std::atomic<int> g_radius{0};
        uint8_t *g_ram = nullptr;
        PS2Runtime::RecompiledFunction g_choose = nullptr, g_areaForPosition = nullptr, g_swap = nullptr, g_drop = nullptr,
                                       g_flush = nullptr, g_draw = nullptr, g_sector = nullptr, g_levelLoad = nullptr;
        constexpr uint32_t kSectorDraw = 0x00240ED8u; // FUN_00240ED8(level, record)
        constexpr uint32_t kPacketBuffer = 0x00520F90u; // the frame's packet buffer (FUN_00476770: its use)
        std::atomic<uint32_t> g_sectorCalls{0}, g_sectorSkips{0}, g_bufferPeak{0};
        double g_costUs = 0.0, g_costPeakUs = 0.0;
        uint64_t g_costPeakFrame = 0;
        struct Cost // adds its scope's time to the frame's
        {
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            ~Cost() { g_costUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count(); }
        };
        std::recursive_mutex g_mutex;

        // ---- guest memory ----
        bool valid(uint32_t a, uint32_t n = 4)
        {
            a &= 0x1FFFFFFFu;
            return a >= 0x00100000u && a < PS2_RAM_SIZE && n <= PS2_RAM_SIZE - a;
        }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_ram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        uint16_t rd16(uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, g_ram + (a & PS2_RAM_MASK), 2);
            return v;
        }
        uint8_t rd8(uint32_t a) { return g_ram[a & PS2_RAM_MASK]; }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_ram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & PS2_RAM_MASK), &v, 4); }
        void wr16(uint32_t a, uint16_t v) { std::memcpy(g_ram + (a & PS2_RAM_MASK), &v, 2); }
        void wrf(uint32_t a, float v) { std::memcpy(g_ram + (a & PS2_RAM_MASK), &v, 4); }

        float half(uint16_t h) // the records' centres and radii (FUN_00471E20)
        {
            const uint32_t sign = uint32_t(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
            uint32_t bits;
            if (exp == 0)
                bits = sign; // (no subnormals in the records)
            else if (exp == 31)
                bits = sign | 0x7F800000u | (man << 13);
            else
                bits = sign | ((exp + 112u) << 23) | (man << 13);
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        // FUN_00241F00's cell centre
        void centre(int x, int y, float &cx, float &cy)
        {
            cx = ((float(x) + 0.5f) - float(y & 1) * 0.5f) * 125.0f - 2400.0f;
            cy = (float(y) + 0.5f) * 108.25f - 2000.0f;
        }

        // FUN_002287E8: is the hour inside [from, to) (wrapping at midnight)
        bool hourWindow(uint8_t from, uint8_t to)
        {
            const uint8_t h = rd8(kHour);
            if (to < from)
            {
                if (from <= h)
                    return true;
            }
            else if (h < from)
                return false;
            return h < to;
        }

        // ---- the port's memory for the cells: first fit, frees held back a few frames ----
        struct Arena
        {
            std::map<uint32_t, uint32_t> free; // start -> bytes
            struct Pending
            {
                uint32_t at, bytes;
                uint64_t frame;
            };
            std::vector<Pending> pending;
            uint32_t used = 0;

            uint32_t largest() const
            {
                uint32_t n = 0;
                for (const auto &[at, bytes] : free)
                    n = std::max(n, bytes);
                return n;
            }
            void reset()
            {
                free.clear();
                pending.clear();
                free[vcs::memory::kHdWorldBase] = vcs::memory::kHdWorldSize;
                used = 0;
            }
            uint32_t alloc(uint32_t bytes)
            {
                bytes = (bytes + 127u) & ~127u;
                for (auto it = free.begin(); it != free.end(); ++it)
                    if (it->second >= bytes)
                    {
                        const uint32_t at = it->first, rest = it->second - bytes;
                        free.erase(it);
                        if (rest)
                            free[at + bytes] = rest;
                        used += bytes;
                        return at;
                    }
                return 0;
            }
            void release(uint32_t at, uint32_t bytes, uint64_t frame)
            {
                if (at)
                    pending.push_back({at, (bytes + 127u) & ~127u, frame});
            }
            void settle(uint64_t frame)
            {
                for (size_t i = 0; i < pending.size();)
                {
                    if (frame < pending[i].frame + kFreeDelay)
                    {
                        ++i;
                        continue;
                    }
                    const Pending p = pending[i];
                    pending[i] = pending.back();
                    pending.pop_back();
                    used -= p.bytes;
                    auto next = free.emplace(p.at, p.bytes).first;
                    auto after = std::next(next);
                    if (after != free.end() && next->first + next->second == after->first)
                    {
                        next->second += after->second;
                        free.erase(after);
                    }
                    if (next != free.begin())
                    {
                        auto before = std::prev(next);
                        if (before->first + before->second == next->first)
                        {
                            before->second += next->second;
                            free.erase(next);
                        }
                    }
                }
            }
        };

        struct Piece
        {
            uint32_t at = 0, bytes = 0;
            std::vector<std::pair<uint32_t, uint32_t>> pairs; // gid, geometry
        };

        struct Area
        {
            int ax = 0, ay = 0, users = 0;
            Piece piece;
        };

        struct Cell
        {
            int x = 0, y = 0;
            float cx = 0, cy = 0;
            Piece list;                                // the cell's piece: its sector list
            std::vector<Piece> conditional;            // pieces its list switches on (state, hour)
            uint64_t area = 0;                         // key into g_areas
            std::unordered_map<uint32_t, uint32_t> geometry; // gid -> our geometry (cell, conditional, area pieces)
            std::bitset<0x8000> ids;                  // the sector ids in its list
            uint32_t owner = 0, areaOwner = 0;         // the table entries' owner tags of ours
            std::vector<std::array<float, 2>> hd;      // the centres of its list's detailed sectors (id < kLodIds)
        };

        struct Installed
        {
            uint32_t slot = 0, original = 0, built = 0, z = 0;
        };

        struct Built // a slot's last built list, kept while its inputs are the same
        {
            uint32_t slot = 0, original = 0, cellX = 0, cellY = 0, zBits = 0;
            uint64_t version = 0, epoch = 0, frame = 0;
            uint32_t at = 0;
            int next = 0;
            std::array<uint32_t, kSynthBuffers> buffers{};
        };

        struct Stats
        {
            uint64_t loads = 0, unloads = 0, failures = 0, builds = 0, installs = 0, restores = 0;
            uint32_t added = 0, hiddenLod = 0, ownRecords = 0, coarser = 0;
            // the last frame's sector draws (FUN_00240ED8): calls, those the full packet buffer skipped, its most used
            uint32_t sectors = 0, skipped = 0, bufferPeak = 0;
            // the hooks' own time: the last frame's, and the most of the last 600 frames (microseconds)
            uint32_t costUs = 0, costPeakUs = 0;
        };

        Arena g_arena;
        std::map<uint64_t, std::unique_ptr<Cell>> g_cells; // key: y << 32 | x
        std::map<uint64_t, Area> g_areas;
        std::map<uint64_t, bool> g_empty; // cells without a piece (none on the disc, or a failed load)
        std::vector<Installed> g_installed;
        std::array<Built, 2> g_built{};
        uint64_t g_frame = 0, g_version = 1, g_epoch = 1;
        uint32_t g_level = 0, g_info = 0, g_levelLoaded = 0;
        uint64_t g_imageBase = 0;
        bool g_arenaReady = false;
        Stats g_stats;
        std::unique_ptr<vcs::IsoImage> g_iso;
        bool g_isoFailed = false;

        uint64_t key(int x, int y) { return (uint64_t(uint32_t(y)) << 32) | uint32_t(x); }

        vcs::IsoImage *iso()
        {
            if (!g_iso && !g_isoFailed)
            {
                auto image = std::make_unique<vcs::IsoImage>();
                if (image->open(PS2Runtime::getIoPaths().cdImage))
                    g_iso = std::move(image);
                else
                {
                    g_isoFailed = true;
                    std::fprintf(stderr, "[hdworld] the disc image cannot be opened: no extra cells\n");
                }
            }
            return g_iso.get();
        }

        // ---- the geometry table ----
        uint32_t tableEntry(uint32_t gid) { return rd32(g_info) + gid * 0xCu; }

        // FUN_0023B830: a cached packet onto the level's deferred free ring
        void freePacket(uint32_t packet)
        {
            const uint32_t ring = rd32(g_level + 0x5ACu), index = rd32(g_level + 0x5BCu), size = rd32(g_level + 0x5C0u);
            if (!valid(ring) || size == 0 || (size & (size - 1u)) || !valid(ring + index * 4u))
                return;
            wr32(ring + index * 4u, packet);
            wr32(g_level + 0x5BCu, (index + 1u) & (size - 1u));
        }

        std::unordered_map<uint32_t, uint32_t> g_registered; // gid -> the geometry of ours its table entry holds

        void enter(const Piece &piece, uint32_t owner, Cell &cell)
        {
            for (const auto &[gid, geometry] : piece.pairs)
            {
                cell.geometry[gid] = geometry;
                const uint32_t e = tableEntry(gid);
                if (rd32(e) != 0)
                    continue; // the game's (or another extra cell's)
                g_registered[gid] = geometry;
                if (const uint32_t packet = rd32(e + 4u))
                {
                    freePacket(packet);
                    wr32(e + 4u, 0);
                }
                wr32(e, geometry);
                wr32(e + 8u, owner);
            }
        }

        void leave(const Piece &piece)
        {
            for (const auto &[gid, geometry] : piece.pairs)
            {
                const uint32_t e = tableEntry(gid);
                const auto mine = g_registered.find(gid);
                if (mine != g_registered.end() && mine->second == geometry)
                    g_registered.erase(mine);
                if (rd32(e) != geometry)
                    continue; // the game took the entry over
                wr32(e, 0);
                if (const uint32_t packet = rd32(e + 4u))
                {
                    freePacket(packet);
                    wr32(e + 4u, 0);
                }
                wr32(e + 8u, 0);
            }
        }

        void release(Piece &piece)
        {
            g_arena.release(piece.at, piece.bytes, g_frame);
            piece = Piece{};
        }

        // ---- reading pieces (FUN_002417D0 / FUN_002414E8, then FUN_00324AC0 as FUN_00241278 / FUN_0023BF38 do) ----
        bool readImage(uint32_t fileOffset, uint32_t bytes, uint32_t to)
        {
            vcs::IsoImage *image = iso();
            return image && image->readBytes(g_imageBase + fileOffset, bytes, g_ram + (to & PS2_RAM_MASK));
        }

        // a 'DLRW' piece (a cell's list, a conditional piece): its index entry gives the size, the relocations (base =
        // the piece - 0x20, as FUN_00241278 passes the slot's cursor) and the file offset
        bool loadIndexed(uint32_t entry, Piece &out)
        {
            if (!valid(entry, 0x20u) || rd32(entry) != kDlrw)
                return false;
            const uint32_t bytes = rd32(entry + 8u), relocAt = rd32(entry + 0x10u), relocs = rd32(entry + 0x14u),
                           fileOffset = rd32(entry + 0x18u);
            if (bytes < 0x40u || bytes > 0x200000u || rd16(entry + 0x1Cu) || rd16(entry + 0x1Eu) ||
                relocAt < 0x20u || relocAt + uint64_t(relocs) * 4u > bytes + 0x20u)
                return false;
            const uint32_t at = g_arena.alloc(bytes);
            if (!at)
                return false;
            if (!readImage(fileOffset, bytes, at))
            {
                g_arena.release(at, bytes, 0);
                return false;
            }
            const uint32_t base = at - 0x20u;
            for (uint32_t i = 0; i < relocs; ++i)
            {
                const uint32_t off = rd32(base + relocAt + i * 4u);
                if (off < 0x20u || off + 4u > bytes + 0x20u)
                {
                    g_arena.release(at, bytes, 0);
                    return false;
                }
                wr32(base + off, rd32(base + off) + base);
            }
            out.at = at;
            out.bytes = bytes;
            const uint32_t pairs = rd32(at), count = rd16(at + 4u);
            if (count && (pairs < at || pairs + count * 8u > at + bytes))
            {
                g_arena.release(at, bytes, 0);
                out = Piece{};
                return false;
            }
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t gid = rd32(pairs + i * 8u), geometry = rd32(pairs + i * 8u + 4u);
                if (gid && gid < kMaxGid && geometry >= at && geometry < at + bytes)
                    out.pairs.emplace_back(gid, geometry);
            }
            return true;
        }

        // an area's piece: the header is the piece's own first 0x20 bytes (base = the piece), then the class and global
        // fixups; its pairs at +0x20 ({count, pointer}, u16 gids)
        bool loadArea(uint32_t entry, Piece &out)
        {
            const uint32_t fileOffset = rd32(entry + 4u), bytes = rd32(entry + 8u);
            if (bytes < 0x40u || bytes > 0x200000u)
                return false;
            const uint32_t at = g_arena.alloc(bytes);
            if (!at)
                return false;
            auto fail = [&] {
                g_arena.release(at, bytes, 0);
                out = Piece{};
                return false;
            };
            if (!readImage(fileOffset, bytes, at))
                return fail();
            const uint32_t relocAt = rd32(at + 0x10u), relocs = rd32(at + 0x14u), fixAt = rd32(at + 0x18u);
            const uint32_t classes = rd16(at + 0x1Cu), globals = rd16(at + 0x1Eu);
            if (relocAt + uint64_t(relocs) * 4u > bytes || fixAt + uint64_t(classes + globals) * 4u > bytes)
                return fail();
            for (uint32_t i = 0; i < relocs; ++i)
            {
                const uint32_t off = rd32(at + relocAt + i * 4u);
                if (off + 4u > bytes)
                    return fail();
                wr32(at + off, rd32(at + off) + at);
            }
            for (uint32_t i = 0; i < classes; ++i)
            {
                const uint32_t p = rd32(at + fixAt + i * 4u);
                if (p < at || p + 4u > at + bytes)
                    return fail();
                const uint32_t id = rd32(p);
                uint32_t found = 0;
                for (uint32_t k = 0; k < 64u; ++k)
                {
                    const uint32_t c = rd32(kClassTable + k * 4u);
                    if (!valid(c, 8u))
                        break;
                    if (rd32(c) == id)
                    {
                        found = c;
                        break;
                    }
                }
                if (!found || !valid(rd32(found + 4u)))
                    return fail();
                wr32(p, rd32(rd32(found + 4u)));
            }
            for (uint32_t i = 0; i < globals; ++i)
            {
                const uint32_t p = rd32(at + fixAt + (classes + i) * 4u);
                if (p < at || p + 4u > at + bytes || rd32(p) >= 0x400u)
                    return fail();
                wr32(p, rd32(kGlobals + rd32(p) * 4u));
            }
            out.at = at;
            out.bytes = bytes;
            const uint32_t count = rd32(at + 0x20u), pairs = rd32(at + 0x24u);
            if (count > 0x4000u || (count && (pairs < at || pairs + count * 8u > at + bytes)))
                return fail();
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t gid = rd16(pairs + i * 8u), geometry = rd32(pairs + i * 8u + 4u);
                if (gid && geometry >= at && geometry < at + bytes)
                    out.pairs.emplace_back(gid, geometry);
            }
            return true;
        }

        // the cell's index entry (FUN_002415B0: rows of 8 bytes at info + 4: {first entry, first x}, the next row's first
        // entry ends it)
        uint32_t cellEntry(int x, int y)
        {
            if (y < 0 || y >= 0x24 || x < 0)
                return 0;
            const uint32_t row = g_info + 4u + uint32_t(y) * 8u, first = rd32(row), firstX = rd32(row + 4u), end = rd32(row + 8u);
            if (int(firstX) > x)
                return 0;
            const uint32_t e = first + uint32_t(x - int(firstX)) * 0x20u;
            return e < end ? e : 0;
        }

        uint32_t areaEntry(int ax, int ay)
        {
            const uint32_t count = rd32(g_info + 0x2D0u), list = rd32(g_info + 0x2D4u);
            if (count > 0x400u || !valid(list, count * 0x10u))
                return 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = list + i * 0x10u;
                if (rd16(e) == ax && rd16(e + 2u) == ay)
                    return rd32(e + 8u) ? e : 0;
            }
            return 0;
        }

        // FUN_0023D600's test: is a conditional piece (or a switched sector, FUN_0023D740) on
        bool switchedOn(uint8_t kind, uint8_t value)
        {
            if (kind == 0xFFu)
                return true;
            if (!(kind & 0x80u))
                return rd32(g_level + 0x418u + kind * 4u) == value;
            return hourWindow(value, kind & 0x7Fu);
        }

        void enterCell(Cell &cell)
        {
            enter(cell.list, cell.owner, cell);
            for (const Piece &p : cell.conditional)
                enter(p, cell.owner, cell);
            const auto a = g_areas.find(cell.area);
            if (a != g_areas.end())
                enter(a->second.piece, cell.areaOwner, cell);
        }

        // entries emptied (one of ours unloaded, or the game dropped its copy of a gid we also hold): ours again
        bool g_reenter = false;
        void reenterAll()
        {
            for (auto &[k, cell] : g_cells)
                enterCell(*cell);
            g_reenter = false;
        }

        void forget(Cell &cell)
        {
            leave(cell.list);
            for (Piece &p : cell.conditional)
                leave(p);
            release(cell.list);
            for (Piece &p : cell.conditional)
                release(p);
            auto a = g_areas.find(cell.area);
            if (a != g_areas.end() && --a->second.users <= 0)
            {
                leave(a->second.piece);
                release(a->second.piece);
                g_areas.erase(a);
            }
        }

        bool load(int x, int y)
        {
            const uint32_t entry = cellEntry(x, y);
            if (!entry || rd32(entry) != kDlrw || rd32(entry + 8u) == 0)
                return false;
            auto cell = std::make_unique<Cell>();
            cell->x = x;
            cell->y = y;
            centre(x, y, cell->cx, cell->cy);
            const uint32_t owner = kOwnerTag | uint32_t(y) << 16 | uint32_t(x);
            if (!loadIndexed(entry, cell->list))
                return false;
            const uint32_t list = cell->list.at;
            // conditional pieces (FUN_0023D600): {kind, value, piece index} at the list's +0x30, count +0x2C
            const uint32_t conds = rd16(list + 0x2Cu), condAt = rd32(list + 0x30u);
            if (conds && conds < 0x100u && condAt >= list && condAt + conds * 4u <= list + cell->list.bytes)
                for (uint32_t i = 0; i < conds; ++i)
                {
                    const uint32_t c = condAt + i * 4u;
                    if (!switchedOn(rd8(c), rd8(c + 1u)))
                        continue;
                    Piece p;
                    if (loadIndexed(rd32(g_info + 4u) + uint32_t(rd16(c + 2u)) * 0x20u, p))
                        cell->conditional.push_back(std::move(p));
                }
            // its area (FUN_0023BDF8: cell / 3)
            const int ax = x / 3, ay = y / 3;
            cell->area = key(ax, ay);
            auto a = g_areas.find(cell->area);
            if (a == g_areas.end())
            {
                Area area;
                area.ax = ax;
                area.ay = ay;
                if (const uint32_t ae = areaEntry(ax, ay))
                    loadArea(ae, area.piece);
                a = g_areas.emplace(cell->area, std::move(area)).first;
            }
            ++a->second.users;
            cell->owner = owner;
            cell->areaOwner = kOwnerTag | 0x08000000u | uint32_t(ay) << 16 | uint32_t(ax);
            enterCell(*cell);
            // its records: the ids, where its detail is
            const uint32_t b0 = rd32(list + 8u), b8 = rd32(list + 0x28u);
            if (b0 >= list && b8 >= b0 && b8 <= list + cell->list.bytes && (b8 - b0) % kRecord == 0)
                for (uint32_t r = b0; r < b8; r += kRecord)
                {
                    const uint16_t id = rd16(r) & 0x7FFFu;
                    cell->ids.set(id);
                    if (id >= 0x20u && id < kLodIds)
                        cell->hd.push_back({half(rd16(r + 4u)), half(rd16(r + 6u))});
                }
            g_cells[key(x, y)] = std::move(cell);
            ++g_stats.loads;
            ++g_version;
            return true;
        }

        void dropAll(bool tableValid)
        {
            for (auto &[k, cell] : g_cells)
            {
                if (tableValid)
                    forget(*cell);
                else
                {
                    release(cell->list);
                    for (Piece &p : cell->conditional)
                        release(p);
                }
            }
            if (!tableValid)
                for (auto &[k, a] : g_areas)
                    release(a.piece);
            g_cells.clear();
            g_areas.clear();
            g_empty.clear();
            g_registered.clear();
            ++g_version;
        }

        // ---- the built lists ----
        // the game's list back in every slot that has ours; the records' z moved by the draw (FUN_0023E748's start: slot
        // + 0x14 against the origin's z) moved in the game's records the same way
        void restore()
        {
            for (const Installed &i : g_installed)
            {
                if (!valid(i.slot, 0x24u) || rd32(i.slot + 0x20u) != i.built)
                    continue;
                wr32(i.slot + 0x20u, i.original);
                const uint32_t z = rd32(i.slot + 0x14u);
                if (z != i.z)
                {
                    float before, after;
                    std::memcpy(&before, &i.z, 4);
                    std::memcpy(&after, &z, 4);
                    const uint32_t b0 = rd32(i.original + 8u), b8 = rd32(i.original + 0x28u);
                    for (uint32_t r = b0; r < b8 && valid(r, kRecord); r += kRecord)
                        wrf(r + 0x48u, rdf(r + 0x48u) + (before - after));
                }
                ++g_stats.restores;
            }
            g_installed.clear();
        }

        // points on a 64 m grid: is one inside a circle (with a test on its tag)
        struct Grid
        {
            static constexpr float kSize = 64.0f;
            struct Point
            {
                float x, y;
                int tag;
            };
            std::unordered_map<int64_t, std::vector<Point>> cells;

            static int64_t at(int gx, int gy) { return (int64_t(gx) << 32) ^ uint32_t(gy); }
            void add(float x, float y, int tag)
            {
                cells[at(int(std::floor(x / kSize)), int(std::floor(y / kSize)))].push_back({x, y, tag});
            }
            template <typename Test = bool (*)(int)>
            bool any(float x, float y, float radius, Test test = [](int) { return true; }) const
            {
                if (!(radius > 0.0f) || radius > 2000.0f)
                    return false;
                const int x0 = int(std::floor((x - radius) / kSize)), x1 = int(std::floor((x + radius) / kSize));
                const int y0 = int(std::floor((y - radius) / kSize)), y1 = int(std::floor((y + radius) / kSize));
                for (int gy = y0; gy <= y1; ++gy)
                    for (int gx = x0; gx <= x1; ++gx)
                    {
                        const auto it = cells.find(at(gx, gy));
                        if (it == cells.end())
                            continue;
                        for (const Point &p : it->second)
                            if ((p.x - x) * (p.x - x) + (p.y - y) * (p.y - y) < radius * radius && test(p.tag))
                                return true;
                    }
                return false;
            }
        };

        struct Rec
        {
            uint16_t id;
            uint32_t from;
            float dx, dy, dz;
            int hide; // -1 keep, 0 show, 1 hide
        };

        // the switched sectors (FUN_0023D740): the last entry naming the id decides
        int switched(uint16_t id)
        {
            const uint32_t count = rd32(g_info + 0x2B8u), list = rd32(g_info + 0x2BCu);
            int hide = -1;
            if (count > 0x1000u || !valid(list, count * 4u))
                return hide;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = list + i * 4u;
                if (rd16(e + 2u) == id)
                    hide = switchedOn(rd8(e), rd8(e + 1u)) ? 0 : 1;
            }
            return hide;
        }

        // the list drawn for a slot: the game's records (low detail that an extra cell's detail replaces hidden), and the
        // extra cells' detailed records moved to the slot's cell
        uint32_t build(Built &b, uint32_t slot, uint32_t original)
        {
            const uint32_t b0 = rd32(original + 8u), b8 = rd32(original + 0x28u);
            if (b8 < b0 || (b8 - b0) % kRecord || (b8 - b0) / kRecord > 0x2000u || !valid(b0, b8 - b0))
                return 0;
            std::array<uint32_t, 9> bucket{};
            for (int k = 0; k < 9; ++k)
            {
                bucket[k] = rd32(original + 8u + k * 4u);
                if (bucket[k] < b0 || bucket[k] > b8 || (k && bucket[k] < bucket[k - 1]))
                    return 0;
            }
            const int sx = int(rd32(slot + 8u)), sy = int(rd32(slot + 0xCu));
            float scx, scy;
            centre(sx, sy, scx, scy);
            const float zShift = rdf(slot + 0x14u);
            const uint32_t table = rd32(g_info);

            std::bitset<0x8000> ids;
            for (uint32_t r = b0; r < b8; r += kRecord)
                ids.set(rd16(r) & 0x7FFFu);

            // the extra cells, nearest the camera first
            const float camX = rdf(kCameraPos), camY = rdf(kCameraPos + 4u);
            std::vector<Cell *> extra;
            for (auto &[k, c] : g_cells)
                if (!(c->x == sx && c->y == sy))
                    extra.push_back(c.get());
            std::sort(extra.begin(), extra.end(), [&](const Cell *a, const Cell *c) {
                return std::hypot(a->cx - camX, a->cy - camY) < std::hypot(c->cx - camX, c->cy - camY);
            });

            // Sector ids are the world's (one centre each, whichever list names them). Detailed sectors (id < kLodIds) of
            // every list are drawn, once each. A low-detail sector is left out where a list leaves it out for its own
            // detail - the rule the lists were made by (9h: replaced where that list's detailed sectors lie inside it):
            // otherwise a neighbour's coarse sectors for the places it sees from afar cover the slot's detail.
            Grid hd; // every list's detailed sectors; tag 0 the slot's, i + 1 extra[i]'s
            for (uint32_t r = b0; r < b8; r += kRecord)
                if (const uint16_t id = rd16(r) & 0x7FFFu; id >= 0x20u && id < kLodIds)
                    hd.add(half(rd16(r + 4u)), half(rd16(r + 6u)), 0);
            for (size_t i = 0; i < extra.size(); ++i)
                for (const auto &p : extra[i]->hd)
                    hd.add(p[0], p[1], int(i + 1));
            auto replaced = [&](uint16_t id, uint32_t r) {
                const float x = half(rd16(r + 4u)), y = half(rd16(r + 6u)), radius = half(rd16(r + 0xAu));
                return hd.any(x, y, radius, [&](int tag) { return tag == 0 ? !ids.test(id) : !extra[size_t(tag - 1)]->ids.test(id); });
            };
            // a gid's table entry holds geometry we know is there: resident (no piece of the list has it), ours, or the
            // game's copy while the slot that entered it still holds its cell / area
            auto drawable = [&](const Cell *c, uint16_t gid) {
                const uint32_t e = table + gid * 0xCu, geometry = rd32(e);
                if (!geometry)
                    return false;
                if (!c->geometry.count(gid))
                    return true;
                if (const auto mine = g_registered.find(gid); mine != g_registered.end() && mine->second == geometry)
                    return true;
                const uint32_t owner = rd32(e + 8u);
                for (int k = 0; k < 3; ++k)
                {
                    if ((owner >> 28) == 4u)
                    {
                        const uint32_t cs = g_level + 0x2Cu + uint32_t(k) * 0xA0u;
                        if (rd32(cs + 0x18u) && (rd32(cs + 0xCu) << 16 | rd32(cs + 8u)) == (owner & 0x0FFFFFFFu))
                            return true;
                    }
                    else if ((owner >> 28) == 8u)
                    {
                        const uint32_t as = g_level + 0x20Cu + uint32_t(k) * 0x14u;
                        if (rd32(as) && (rd32(as + 0xCu) << 16 | rd32(as + 8u)) == (owner & 0x0FFFFFFFu))
                            return true;
                    }
                }
                return false;
            };

            std::array<std::vector<Rec>, 8> out;
            uint32_t hiddenLod = 0, added = 0, coarser = 0;
            for (int k = 0; k < 8; ++k)
                for (uint32_t r = bucket[k]; r < bucket[k + 1]; r += kRecord)
                {
                    Rec rec{uint16_t(rd16(r) & 0x7FFFu), r, 0.0f, 0.0f, 0.0f, -1};
                    if (rec.id >= kLodIds && !(rd16(r) & 0x8000u) && rd32(r + 0xCu) == 0 && replaced(rec.id, r))
                    {
                        rec.hide = 1;
                        ++hiddenLod;
                    }
                    out[k].push_back(rec);
                }
            std::bitset<0x8000> taken = ids;
            const uint32_t room = (kSynthBytes - 0x40u) / kRecord;
            uint32_t total = (b8 - b0) / kRecord;
            for (const Cell *c : extra)
            {
                const uint32_t list = c->list.at;
                for (int k = 0; k < 8; ++k)
                {
                    const uint32_t from = rd32(list + 8u + k * 4u), to = rd32(list + 0xCu + k * 4u);
                    if (from < list || to < from || to > list + c->list.bytes)
                        continue;
                    for (uint32_t r = from; r < to && total < room; r += kRecord)
                    {
                        const uint16_t id = rd16(r) & 0x7FFFu, gid = rd16(r + 2u);
                        if (id < 0x20u || taken.test(id) || rd32(r + 0xCu) || !drawable(c, gid))
                            continue;
                        if (id >= kLodIds && replaced(id, r))
                        {
                            ++coarser;
                            continue;
                        }
                        taken.set(id);
                        int hide = switched(id);
                        if (hide < 0)
                            hide = 0;
                        out[k].push_back({id, r, c->cx - scx, c->cy - scy, -zShift, hide});
                        ++added;
                        ++total;
                    }
                }
            }
            g_stats.coarser = coarser;

            // write it: header (the game's, the buckets ours), records by bucket sorted by id (FUN_0023E748's merge)
            const uint32_t at = b.buffers[b.next];
            b.next = (b.next + 1) % kSynthBuffers;
            std::memcpy(g_ram + (at & PS2_RAM_MASK), g_ram + (original & PS2_RAM_MASK), 0x34u);
            uint32_t cursor = at + 0x40u;
            for (int k = 0; k < 8; ++k)
            {
                wr32(at + 8u + k * 4u, cursor);
                std::stable_sort(out[k].begin(), out[k].end(), [](const Rec &a, const Rec &c) { return a.id < c.id; });
                for (const Rec &rec : out[k])
                {
                    std::memcpy(g_ram + (cursor & PS2_RAM_MASK), g_ram + (rec.from & PS2_RAM_MASK), kRecord);
                    if (rec.dx != 0.0f || rec.dy != 0.0f || rec.dz != 0.0f)
                    {
                        wrf(cursor + 0x40u, rdf(cursor + 0x40u) + rec.dx);
                        wrf(cursor + 0x44u, rdf(cursor + 0x44u) + rec.dy);
                        wrf(cursor + 0x48u, rdf(cursor + 0x48u) + rec.dz);
                    }
                    if (rec.hide >= 0)
                        wr16(cursor, uint16_t((rd16(cursor) & 0x7FFFu) | (rec.hide ? 0x8000u : 0u)));
                    cursor += kRecord;
                }
            }
            wr32(at + 0x28u, cursor);
            g_stats.added = added;
            g_stats.hiddenLod = hiddenLod;
            g_stats.ownRecords = (b8 - b0) / kRecord;
            ++g_stats.builds;
            return at;
        }

        bool levelUsable()
        {
            const uint32_t level = rd32(kLevelPtr);
            if (!valid(level, 0x600u))
                return false;
            const uint32_t info = rd32(level + 0x1Cu), loaded = rd32(level + 0x18u);
            if (level != g_level || info != g_info || loaded != g_levelLoaded)
            {
                if (g_level)
                    dropAll(false); // another level's data: the table is not what we entered into
                g_level = level;
                g_info = info;
                g_levelLoaded = loaded;
                g_imageBase = 0;
            }
            if (loaded != rd32(kLevelId))
                return false; // the other island's data is about to be read
            if (!valid(info, 0x300u) || !valid(rd32(info), 0xC000u))
                return false;
            const uint32_t handle = rd32(level + 0x278u);
            if (handle >= 64u)
                return false;
            g_imageBase = uint64_t(rd32(kFileTable + handle * 0x14u + 4u)) * vcs::IsoImage::kSectorSize;
            return g_imageBase != 0;
        }

        // every frame, before the game's chooser: the extra cells around the camera
        void update()
        {
            ++g_frame;
            if (!g_arenaReady)
            {
                g_arena.reset();
                for (Built &b : g_built)
                    for (uint32_t &buf : b.buffers)
                        buf = g_arena.alloc(kSynthBytes);
                g_arenaReady = true;
            }
            g_arena.settle(g_frame);
            const int radius = std::clamp(g_radius.load(std::memory_order_relaxed), 0, 3);
            if (!levelUsable())
                return;
            const uint32_t current = rd32(g_level + 0x248u);
            // nothing extra while a cell is forced (level + 0x260: interiors, cutscenes) or the current cell is not in
            if (radius == 0 || rd32(g_level + 0x260u) != 0 || !valid(current, 0x40u) || rd32(current + 0x18u) == 0)
            {
                if (!g_cells.empty())
                    dropAll(true);
                return;
            }
            const float camX = rdf(kCameraPos), camY = rdf(kCameraPos + 4u);
            if (!std::isfinite(camX) || !std::isfinite(camY))
                return;
            const float reach = kReach[radius];
            if (g_reenter)
            {
                reenterAll();
                ++g_version;
            }
            // too far now: dropped
            for (auto it = g_cells.begin(); it != g_cells.end();)
            {
                if (std::hypot(it->second->cx - camX, it->second->cy - camY) > reach + kKeepMargin)
                {
                    forget(*it->second);
                    it = g_cells.erase(it);
                    ++g_stats.unloads;
                    ++g_version;
                    g_reenter = true;
                }
                else
                    ++it;
            }
            // the nearest wanted one not in yet (one a frame: ~0.2-0.7 MB copied from the mapped disc image)
            const int cy0 = int((camY + 2000.0f) * 0.009237875f);
            const int cx0 = int((camX + 2400.0f) * 0.008f + float(cy0 & 1) * 0.5f);
            const int span = int(reach / 100.0f) + 2;
            float best = 1e9f;
            int bx = 0, by = 0;
            for (int y = cy0 - span; y <= cy0 + span; ++y)
                for (int x = cx0 - span; x <= cx0 + span; ++x)
                {
                    if (x < 0 || y < 0 || y >= 0x24)
                        continue;
                    if (x == int(rd32(current + 8u)) && y == int(rd32(current + 0xCu)))
                        continue;
                    const uint64_t k = key(x, y);
                    if (g_cells.count(k) || g_empty.count(k))
                        continue;
                    float cx, cy;
                    centre(x, y, cx, cy);
                    const float d = std::hypot(cx - camX, cy - camY);
                    if (d <= reach && d < best)
                        best = d, bx = x, by = y;
                }
            // (a full arena waits for frees: the largest pieces are ~0.9 MB)
            if (best < 1e9f && g_arena.largest() >= 0x180000u && !load(bx, by))
            {
                g_empty[key(bx, by)] = true;
                ++g_stats.failures;
            }
        }

        // at the world draw: our lists in the current slot (and the previous one while they cross-fade)
        void install()
        {
            if (g_cells.empty() || !valid(g_level, 0x600u) || rd32(kLevelPtr) != g_level || rd32(g_level + 0x260u) != 0 ||
                rd32(g_level + 0x18u) != g_levelLoaded || g_levelLoaded != rd32(kLevelId))
                return;
            const uint32_t current = rd32(g_level + 0x248u), previous = rd32(g_level + 0x250u);
            const bool fading = int32_t(rd32(g_level + 0x264u)) >= 1 && valid(previous, 0x40u) && int32_t(rd32(previous + 8u)) >= 1;
            const std::pair<uint32_t, uint32_t> slots[2] = {{current, rd32(g_level + 0x24Cu)},
                                                            {fading ? previous : 0u, rd32(g_level + 0x254u)}};
            for (int i = 0; i < 2; ++i)
            {
                const uint32_t slot = slots[i].first;
                if (!valid(slot, 0x40u) || rd32(slot + 0x1Cu) == 0)
                    continue;
                const uint32_t piece = rd32(slot + 0x20u);
                bool mine = false;
                for (const Installed &in : g_installed)
                    mine |= in.slot == slot && in.built == piece;
                if (mine || !valid(piece, 0x34u))
                    continue;
                Built &b = g_built[i];
                const uint32_t zBits = rd32(slot + 0x14u);
                if (!(b.at && b.slot == slot && b.original == piece && b.cellX == rd32(slot + 8u) && b.cellY == rd32(slot + 0xCu) &&
                      b.zBits == zBits && b.version == g_version && b.epoch == g_epoch && g_frame < b.frame + 30u))
                {
                    b.at = build(b, slot, piece);
                    b.slot = slot;
                    b.original = piece;
                    b.cellX = rd32(slot + 8u);
                    b.cellY = rd32(slot + 0xCu);
                    b.zBits = zBits;
                    b.version = g_version;
                    b.epoch = g_epoch;
                    b.frame = g_frame;
                }
                if (!b.at)
                    continue;
                wr32(slot + 0x20u, b.at);
                g_installed.push_back({slot, piece, b.at, zBits});
                ++g_stats.installs;
            }
        }

        // ---- hooks: the work before the original (DECISIONS.md, hooks on recompiled functions) ----
        // FUN_00476770: bytes used in the frame's packet buffer (two of 0xE6670, filled from both ends)
        uint32_t bufferUsed()
        {
            const uint32_t base = kPacketBuffer + rd32(kPacketBuffer + 0x14u) * 0xE6670u + 0x20u;
            return ((rd32(kPacketBuffer + 0x18u) - (base - 0x10u)) >> 4) * 0x10u + (base + 0xE6670u - rd32(kPacketBuffer + 0x1Cu));
        }

        // counts only (FUN_00240ED8 returns at once when fewer than 0x12C00 bytes are free)
        PS2_GUEST_CC void sectorHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_ram = rdram;
            const uint32_t used = bufferUsed();
            g_sectorCalls.fetch_add(1, std::memory_order_relaxed);
            if (0xE6670u - used < 0x12C00u)
                g_sectorSkips.fetch_add(1, std::memory_order_relaxed);
            if (used > g_bufferPeak.load(std::memory_order_relaxed))
                g_bufferPeak.store(used, std::memory_order_relaxed);
            g_sector(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void chooseHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                g_ram = rdram;
                g_stats.sectors = g_sectorCalls.exchange(0);
                g_stats.skipped = g_sectorSkips.exchange(0);
                g_stats.bufferPeak = g_bufferPeak.exchange(0);
                g_stats.costUs = uint32_t(g_costUs);
                if (g_costUs > g_costPeakUs || g_frame > g_costPeakFrame + 600u)
                {
                    g_costPeakUs = g_costUs;
                    g_costPeakFrame = g_frame;
                }
                g_stats.costPeakUs = uint32_t(g_costPeakUs);
                g_costUs = 0.0;
                Cost cost;
                restore();
                update();
            }
            g_choose(rdram, ctx, runtime);
        }
        template <PS2Runtime::RecompiledFunction *Original>
        PS2_GUEST_CC void restoreHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                g_ram = rdram;
                restore();
                g_reenter = !g_cells.empty(); // it may drop the game's copies of gids we hold too
            }
            (*Original)(rdram, ctx, runtime);
        }
        // before the level's data is read again: our entries out of the table while it is still the one we entered
        // them into (as the game drops its own slots there)
        PS2_GUEST_CC void levelLoadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                g_ram = rdram;
                restore();
                if (!g_cells.empty() && valid(g_level, 0x600u) && rd32(g_level + 0x1Cu) == g_info &&
                    rd32(g_level + 0x18u) == g_levelLoaded)
                    dropAll(true);
                else if (!g_cells.empty())
                    dropAll(false);
                g_levelLoaded = ~0u; // read again at the next frame
            }
            g_levelLoad(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void drawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_radius.load(std::memory_order_relaxed) > 0 || !g_installed.empty())
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                g_ram = rdram;
                Cost cost;
                install();
            }
            g_draw(rdram, ctx, runtime);
        }
    } // namespace

    void registerSettings()
    {
        settings::Spec s;
        s.key = "world.hd_radius";
        s.page = "Graphics";
        s.section = "Distance and population (experimental)";
        s.label = "Detailed city distance";
        s.help = "How far the city's detailed buildings reach. The game draws them ~260 m around the camera and its "
                 "low-detail image beyond; more reads the cells around into extra memory and draws them too.";
        s.type = settings::Type::Choice;
        s.choices = {{"Original (~260 m)", 0}, {"~450 m", 1}, {"~580 m", 2}, {"~720 m", 3}};
        s.defaultValue = 0;
        s.env = "VCS_HD_RADIUS";
        s.onChange = [](int v) { g_radius.store(std::clamp(v, 0, 3), std::memory_order_relaxed); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        struct Hook
        {
            uint32_t at;
            PS2Runtime::RecompiledFunction *original;
            PS2Runtime::RecompiledFunction hook;
        } hooks[] = {
            {kChooseArea, &g_choose, &chooseHook},
            {kAreaForPosition, &g_areaForPosition, &restoreHook<&g_areaForPosition>},
            {kSwapArea, &g_swap, &restoreHook<&g_swap>},
            {kDropPrevious, &g_drop, &restoreHook<&g_drop>},
            {kFlush, &g_flush, &restoreHook<&g_flush>},
            {kLevelLoad, &g_levelLoad, &levelLoadHook},
            {kWorldDraw, &g_draw, &drawHook},
            {kSectorDraw, &g_sector, &sectorHook},
        };
        for (const Hook &h : hooks)
            if (!(*h.original = runtime.lookupFunction(h.at)))
            {
                std::fprintf(stderr, "[hdworld] FUN_%08X not found: the detailed city stays the game's\n", h.at);
                return;
            }
        for (const Hook &h : hooks)
            runtime.replaceFunction(h.at, h.hook);

        // hdworld -> ok radius=<n> cells=<n> areas=<n> mem=<KB> added=<records> hidden=<low detail> own=<records> ...
        ps2control::addCommand("hdworld", [](const std::string &, ps2control::Reply reply) {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            char text[512];
            std::snprintf(text, sizeof(text),
                          "ok radius=%d cells=%zu areas=%zu empty=%zu mem=%u added=%u coarser=%u hidden=%u own=%u sectors=%u skipped=%u "
                          "buffer_peak=%u cost_us=%u cost_peak_us=%u loads=%llu unloads=%llu failures=%llu builds=%llu installs=%llu restores=%llu",
                          g_radius.load(), g_cells.size(), g_areas.size(), g_empty.size(), g_arena.used / 1024u, g_stats.added,
                          g_stats.coarser, g_stats.hiddenLod, g_stats.ownRecords, g_stats.sectors, g_stats.skipped,
                          g_stats.bufferPeak, g_stats.costUs, g_stats.costPeakUs, (unsigned long long)g_stats.loads,
                          (unsigned long long)g_stats.unloads, (unsigned long long)g_stats.failures,
                          (unsigned long long)g_stats.builds, (unsigned long long)g_stats.installs,
                          (unsigned long long)g_stats.restores);
            reply(text);
        });
    }
}
