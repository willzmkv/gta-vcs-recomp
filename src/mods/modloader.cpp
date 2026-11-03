// The mod loader - see mods/modloader.h and docs/MODS.md "The mod loader".
//
// A mod is a folder in modloader/ (any name; a leading '.' hides it). modloader/modloader.ini is read in Mod Loader's
// own format, so its knobs mean the same here: [Folder.Config] Profile, and the profile's [Profiles.<P>.Config]
// IgnoreAllMods, [Profiles.<P>.Priority] Mod=1..100 (50 by default, 0 ignores it; a higher one wins a file two mods
// have) and [Profiles.<P>.IgnoreMods] (wildcards). F2 > Mods switches each mod, all of them, or their textures; a
// change is applied at the next host frame, while the game runs (gsHwSetTextureReplacements).
//
// Test runs (hidden) stay authentic: they load mods only with VCS_MODS=1, and the texture packs outside modloader/
// (VCS_TEXTURE_PACK, textures/SLUS-21590) only with VCS_TEXTURE_REPLACEMENTS=1 - or both with VCS_TEST_MODS=1, which
// leaves the switches free for the `setting` command (an environment value fixes its setting).
#include "mods/modloader.h"
#include "mods/rwconv.h"

#include "game/vcs_stream.h"
#include "platform/platform.h"
#include "render/texture_replacements.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vcs::mods
{
    namespace
    {
        struct Mod
        {
            std::string name;    // the folder's
            std::string key;     // its switch in vcs.ini: mods.<the name in lower case, other characters as _>
            fs::path path;
            int priority = 50;   // modloader.ini
            size_t textures = 0; // .dds / .png files in it at start (dumps/ aside)
            size_t gameFiles = 0; // game files in it at start (gameFilesIn)
        };

        fs::path g_root, g_exeDir;
        std::vector<Mod> g_mods;
        std::string g_profileName = "Default"; // modloader.ini's profile, as written
        bool g_dirty = true;

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        std::string trim(const std::string &s)
        {
            const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }

        std::string utf8(const fs::path &p)
        {
            const auto u = p.u8string();
            return std::string(u.begin(), u.end());
        }

        // Mod Loader's wildcards: * and ?, case-insensitive
        bool matches(const char *pattern, const char *text)
        {
            for (; *pattern; ++pattern, ++text)
            {
                if (*pattern == '*')
                {
                    for (const char *t = text;; ++t)
                    {
                        if (matches(pattern + 1, t))
                            return true;
                        if (!*t)
                            return false;
                    }
                }
                if (!*text || (*pattern != '?' && std::tolower(static_cast<unsigned char>(*pattern)) !=
                                                       std::tolower(static_cast<unsigned char>(*text))))
                    return false;
            }
            return !*text;
        }

        struct Config
        {
            std::string profile = "default";      // lower case, for matching
            std::string profileName = "Default";  // as written, for new sections
            std::map<std::string, int> priority; // lower-case mod name -> 0..100
            std::vector<std::string> ignore;     // wildcards
            bool ignoreAll = false;
        };

        Config readConfig(const fs::path &ini)
        {
            Config c;
            std::ifstream in(ini);
            if (!in)
                c.ignore = {"_ignore"}; // no file yet: Mod Loader's shipped default
            std::vector<std::pair<std::string, std::string>> lines; // (section, line)
            std::string section, line;
            while (std::getline(in, line))
            {
                if (const size_t comment = line.find(';'); comment != std::string::npos)
                    line.erase(comment);
                line = trim(line);
                if (line.empty())
                    continue;
                if (line.front() == '[' && line.back() == ']')
                    section = lower(trim(line.substr(1, line.size() - 2)));
                else
                    lines.emplace_back(section, line);
            }
            auto field = [](const std::string &l, bool value) {
                const size_t eq = l.find('=');
                return eq == std::string::npos ? (value ? std::string() : trim(l)) : trim(value ? l.substr(eq + 1) : l.substr(0, eq));
            };
            for (const auto &[s, l] : lines)
                if (s == "folder.config" && lower(field(l, false)) == "profile" && !field(l, true).empty())
                {
                    c.profileName = field(l, true);
                    c.profile = lower(c.profileName);
                }
            const std::string p = "profiles." + c.profile + ".";
            for (const auto &[s, l] : lines)
            {
                if (s == p + "priority" && l.find('=') != std::string::npos)
                    c.priority[lower(field(l, false))] = std::atoi(field(l, true).c_str());
                else if (s == p + "ignoremods")
                    c.ignore.push_back(l);
                else if (s == p + "config" && lower(field(l, false)) == "ignoreallmods")
                    c.ignoreAll = lower(field(l, true)) == "true" || field(l, true) == "1";
            }
            return c;
        }

        fs::path iniPath() { return g_root / "modloader.ini"; }

        // A test run writes nothing into the player's modloader/ - only into a folder of its own (VCS_MODLOADER_DIR)
        bool mayWrite()
        {
            const char *dir = std::getenv("VCS_MODLOADER_DIR");
            return !ps2control::hiddenMode() || (dir && *dir);
        }

        // modloader.ini as PC Mod Loader ships it (the parts this port reads), with the installed mods' priorities listed,
        // when the folder has none.
        void ensureIni()
        {
            std::error_code ec;
            if (!mayWrite() || fs::exists(iniPath(), ec))
                return;
            fs::create_directories(g_root, ec);
            std::ofstream out(iniPath());
            out << ";\n"
                   ";           Mod Loader Folder Config File - GTA: Vice City Stories (native port)\n"
                   ";           PC GTA's Mod Loader format. F2 > Mods changes the priorities here; this file can be edited too\n"
                   ";           (F2 > Mods > Rescan reads it again).\n"
                   ";\n"
                   "[Folder.Config]\n"
                   "Profile = Default                 ; Profile to be used to configure mods\n"
                   "\n"
                   "[Profiles.Default.Config]\n"
                   "IgnoreAllMods  = false            ; Ignores all mods. This essentially disables mods loading.\n"
                   "\n"
                   "[Profiles.Default.Priority]\n"
                   "; Defines mods priority (which mod goes into effect when two replace the same file).\n"
                   "; The priority is between 1 and 100, where higher values override lower ones.\n"
                   "; Priority 0 makes the mod be ignored (just like IgnoreMods). The default priority is 50.\n";
            for (const Mod &m : g_mods)
                out << m.name << "=" << m.priority << "\n";
            out << "\n"
                   "[Profiles.Default.IgnoreMods]\n"
                   "; Wildcards of mods to be ignored\n"
                   "_ignore\n";
            std::fprintf(stderr, "[mods] made %s\n", utf8(iniPath()).c_str());
        }

        // `name=value` in [Profiles.<profile>.Priority]: its line replaced, or added at the section's end (the section
        // made if missing); every other line kept as it was
        void writePriority(const std::string &name, int value)
        {
            if (!mayWrite())
                return;
            ensureIni();
            std::vector<std::string> lines;
            {
                std::ifstream in(iniPath());
                std::string line;
                while (std::getline(in, line))
                    lines.push_back(line.size() && line.back() == '\r' ? line.substr(0, line.size() - 1) : line);
            }
            auto content = [](const std::string &l) { return trim(l.substr(0, l.find(';'))); };
            const std::string wanted = lower("profiles." + g_profileName + ".priority");
            size_t section = std::string::npos, end = lines.size();
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::string t = content(lines[i]);
                if (t.size() < 2 || t.front() != '[' || t.back() != ']')
                    continue;
                if (section != std::string::npos && end == lines.size())
                    end = i;
                if (section == std::string::npos && lower(trim(t.substr(1, t.size() - 2))) == wanted)
                    section = i;
            }
            const std::string entry = name + "=" + std::to_string(value);
            bool written = false;
            if (section == std::string::npos)
            {
                lines.push_back("");
                lines.push_back("[Profiles." + g_profileName + ".Priority]");
                lines.push_back(entry);
                written = true;
            }
            for (size_t i = section + 1; !written && i < end; ++i)
            {
                const std::string t = content(lines[i]);
                const size_t eq = t.find('=');
                if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(name))
                {
                    lines[i] = entry;
                    written = true;
                }
            }
            if (!written)
            {
                size_t at = end;
                while (at > section + 1 && trim(lines[at - 1]).empty())
                    --at;
                lines.insert(lines.begin() + std::ptrdiff_t(at), entry);
            }
            const fs::path tmp = iniPath().string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                for (const std::string &l : lines)
                    out << l << "\n";
                if (!out)
                    return;
            }
            std::error_code ec;
            fs::rename(tmp, iniPath(), ec);
            if (ec)
                std::fprintf(stderr, "[mods] could not write %s\n", utf8(iniPath()).c_str());
        }

        // F2 > Mods > Rescan: modloader.ini's priorities again (an edited file), for the known mods
        void reloadPriorities()
        {
            const Config config = readConfig(iniPath());
            g_profileName = config.profileName;
            for (Mod &m : g_mods)
            {
                const auto set = config.priority.find(lower(m.name));
                m.priority = set == config.priority.end() ? 50 : std::clamp(set->second, 1, 100);
            }
        }

        size_t countTextures(const fs::path &folder)
        {
            size_t n = 0;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec))
            {
                if (it->is_directory(ec) && it->path().filename() == "dumps")
                    it.disable_recursion_pending();
                else if (it->is_regular_file(ec))
                {
                    const std::string ext = lower(it->path().extension().string());
                    n += ext == ".dds" || ext == ".png";
                }
            }
            return n;
        }

        // Game files (vcs::setStreamFiles): GTA3PS2.IMG entries - `<model name>.mdl`, `<texture dictionary>.xtx`, or
        // `#<streaming index>.<any>` as tools/img.py extracts the entries it has no name for; loose disc files (.gxt,
        // .scm); PC VC cars - `<model name>.dff`, and the `.txd` of the same name (only beside a .dff: a PC mod's other
        // dictionaries - hud, generic - hold VC's textures, not VCS's); anywhere in the mod (dumps/ aside)
        std::vector<fs::path> gameFilesIn(const fs::path &folder)
        {
            std::vector<fs::path> out;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec))
            {
                if (it->is_directory(ec) && it->path().filename() == "dumps")
                    it.disable_recursion_pending();
                else if (it->is_regular_file(ec))
                {
                    const std::string stem = utf8(it->path().stem()), ext = lower(it->path().extension().string());
                    if (ext == ".mdl" || ext == ".xtx" || ext == ".gxt" || ext == ".scm" || ext == ".dff" || ext == ".txd" ||
                        (stem.size() > 1 && stem[0] == '#'))
                        out.push_back(it->path());
                }
            }
            std::sort(out.begin(), out.end());
            std::vector<std::string> cars; // the .dff names: their .txd go with them
            for (const fs::path &p : out)
                if (lower(p.extension().string()) == ".dff")
                    cars.push_back(lower(utf8(p.stem())));
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [&](const fs::path &p) {
                                         return lower(p.extension().string()) == ".txd" &&
                                                std::find(cars.begin(), cars.end(), lower(utf8(p.stem()))) == cars.end();
                                     }),
                      out.end());
            return out;
        }

        fs::path findRoot(const fs::path &exeDir)
        {
            if (const char *dir = std::getenv("VCS_MODLOADER_DIR"); dir && *dir)
                return fs::u8path(dir);
            std::error_code ec;
            for (const fs::path &candidate : {exeDir / "modloader", exeDir.parent_path() / "modloader"})
                if (fs::is_directory(candidate, ec))
                    return candidate;
            // none yet: where "Open" makes it - the checkout's root when vcs.exe is in its build/
            return fs::exists(exeDir.parent_path() / "tools" / "build.py", ec) ? exeDir.parent_path() / "modloader" : exeDir / "modloader";
        }

        std::string keyFor(const std::string &name)
        {
            std::string key = "mods.";
            for (const unsigned char c : name)
                key += std::isalnum(c) ? char(std::tolower(c)) : '_';
            return key;
        }

        void apply()
        {
            using Source = render::TextureReplacements::Source;
            // a test run: VCS_MODS / VCS_TEXTURE_REPLACEMENTS (which also fix the switch), or VCS_TEST_MODS=1 (switches free)
            const bool allowed = !ps2control::hiddenMode() || std::getenv("VCS_TEST_MODS");
            const bool textures = settings::value("graphics.texture_replacements") != 0 && (allowed || std::getenv("VCS_TEXTURE_REPLACEMENTS"));
            const bool mods = settings::value("mods.enabled") != 0 && (allowed || std::getenv("VCS_MODS"));
            std::vector<Source> sources;
            if (textures)
            {
                if (const char *pack = std::getenv("VCS_TEXTURE_PACK"); pack && *pack)
                    sources.push_back({fs::u8path(pack), 100, "VCS_TEXTURE_PACK"});
                if (fs::path ini = fs::u8path(settings::state("graphics.texture_pack")); !ini.empty())
                    sources.push_back({ini.is_relative() ? g_exeDir / ini : ini, 50, "graphics.texture_pack (vcs.ini)"});
                // a PCSX2 folder beside vcs.exe or in the checkout's root (upstream PR #8's places)
                for (const fs::path &candidate : {g_exeDir / "textures" / "SLUS-21590", g_exeDir.parent_path() / "textures" / "SLUS-21590",
                                                  g_exeDir / "SLUS-21590", g_exeDir.parent_path() / "SLUS-21590"})
                {
                    std::error_code ec;
                    if (fs::is_directory(candidate / "replacements", ec))
                    {
                        sources.push_back({candidate, 50, "textures/SLUS-21590"});
                        break;
                    }
                }
                if (mods)
                    for (const Mod &m : g_mods)
                        if (settings::value(m.key))
                            sources.push_back({m.path, m.priority, m.name});
            }
            std::vector<vcs::StreamFile> files; // game files: Load mods alone governs them (VCS_MOD_FILES=0: none)
            const char *filesOff = std::getenv("VCS_MOD_FILES");
            if (mods && !(filesOff && *filesOff == '0'))
                for (const Mod &m : g_mods)
                    if (settings::value(m.key))
                    {
                        const std::vector<fs::path> list = gameFilesIn(m.path);
                        for (const fs::path &file : list)
                        {
                            const std::string ext = lower(file.extension().string());
                            const bool loose = ext == ".gxt" || ext == ".scm"; // a disc file, by its whole name
                            const bool txd = ext == ".xtx" && file.stem().string()[0] != '#';
                            vcs::StreamFile f{utf8(loose ? file.filename() : file.stem()), utf8(file), m.name, m.priority, txd};
                            if (ext == ".dff" || ext == ".txd")
                            {
                                f.format = ext == ".dff" ? vcs::StreamFile::Format::PcDff : vcs::StreamFile::Format::PcTxd;
                                f.name = vcs::rwconv::vcsName(f.name); // police.dff: VCS's policem
                                for (const fs::path &other : list) // the car's textures: the .txd of its name in the mod
                                    if (ext == ".dff" && lower(other.extension().string()) == ".txd" && lower(utf8(other.stem())) == lower(utf8(file.stem())))
                                    {
                                        f.companion = utf8(other);
                                        break;
                                    }
                            }
                            files.push_back(std::move(f));
                        }
                    }
            vcs::setStreamFiles(files);
            const auto start = std::chrono::steady_clock::now();
            std::shared_ptr<render::TextureReplacements> pack;
            if (!sources.empty())
                pack = std::make_shared<render::TextureReplacements>(sources);
            gsHwSetTextureReplacements(pack);
            std::fprintf(stderr, "[mods] %zu game file(s); textures from %zu source(s)%s, indexed in %.0f ms\n", files.size(), sources.size(),
                         !textures ? " (Replace textures off)" : (!mods ? " (Load mods off)" : ""),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    }

    void registerSettings(const fs::path &exeDir)
    {
        g_exeDir = exeDir;
        g_root = findRoot(exeDir);
        const Config config = readConfig(iniPath());
        g_profileName = config.profileName;
        std::vector<fs::path> folders;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(g_root, ec))
            if (entry.is_directory(ec))
                folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end(), [](const fs::path &a, const fs::path &b) {
            return lower(utf8(a.filename())) < lower(utf8(b.filename()));
        });
        for (const fs::path &folder : folders)
        {
            const std::string name = utf8(folder.filename());
            if (name.empty() || name.front() == '.')
                continue;
            const auto ignored = std::find_if(config.ignore.begin(), config.ignore.end(),
                                              [&](const std::string &w) { return matches(w.c_str(), name.c_str()); });
            const auto set = config.priority.find(lower(name));
            const int priority = set == config.priority.end() ? 50 : std::min(set->second, 100);
            if (config.ignoreAll || ignored != config.ignore.end() || priority <= 0)
            {
                std::fprintf(stderr, "[mods] %s: ignored by modloader.ini\n", name.c_str());
                continue;
            }
            std::string key = keyFor(name);
            while (std::any_of(g_mods.begin(), g_mods.end(), [&](const Mod &m) { return m.key == key; }))
                key += '_';
            g_mods.push_back({name, key, folder, priority, countTextures(folder), gameFilesIn(folder).size()});
        }
        std::fprintf(stderr, "[mods] %zu mod(s) in %s\n", g_mods.size(), utf8(g_root).c_str());
        ensureIni();

        using settings::Type;
        settings::Spec s;
        s.key = "mods.enabled";
        s.page = "Mods";
        s.section = "Mod loader";
        s.label = "Load mods";
        s.help = "Mods are the folders in " + utf8(g_root) + ", one folder a mod, as with PC GTA's Mod Loader: nothing of the "
                 "game's is changed. Off: the game as it is.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_MODS";
        s.onChange = [](int) { g_dirty = true; };
        settings::add(s);

        s = {};
        s.key = "graphics.texture_replacements";
        s.page = "Mods";
        s.section = "Mod loader";
        s.label = "Replace textures";
        s.help = "Texture packs - PCSX2 file names, DDS or PNG (docs/TEXTURE_PACKS.md) - from the mods, textures/SLUS-21590 and "
                 "graphics.texture_pack in vcs.ini. Off: the game's own textures, at once.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_TEXTURE_REPLACEMENTS";
        s.onChange = [](int) { g_dirty = true; };
        settings::add(s);

        for (size_t i = 0; i < g_mods.size(); ++i)
        {
            const Mod &m = g_mods[i];
            s = {};
            s.key = m.key;
            s.page = "Mods";
            s.section = "Installed mods";
            s.label = m.name;
            char what[240];
            if (m.textures || m.gameFiles)
                std::snprintf(what, sizeof(what), "%zu texture files (DDS / PNG), %zu game files (GTA3PS2.IMG entries). The slider is "
                                                  "its priority, 1-100: when two mods have the same file, the higher one wins (kept "
                                                  "in modloader.ini). ", m.textures, m.gameFiles);
            else
                std::snprintf(what, sizeof(what), "Nothing in it this version reads (texture packs, GTA3PS2.IMG entries). ");
            s.help = what + utf8(m.path);
            s.type = Type::Toggle;
            s.defaultValue = 1;
            s.partner = m.key + ".priority";
            s.onChange = [](int) { g_dirty = true; };
            settings::add(s);

            // its priority: the slider beside the switch; the value lives in modloader.ini, not vcs.ini
            s = {};
            s.key = m.key + ".priority";
            s.page = "Mods";
            s.section = "Installed mods";
            s.label = m.name + " priority";
            s.type = Type::Integer;
            s.min = 1;
            s.max = 100;
            s.format = "Priority %d";
            s.defaultValue = 50; // Mod Loader's
            s.hidden = true;
            s.saved = false;
            s.read = [i] { return g_mods[i].priority; };
            s.onChange = [i](int v) {
                Mod &mod = g_mods[i];
                if (mod.priority == v)
                    return;
                mod.priority = v;
                writePriority(mod.name, v);
                g_dirty = true;
            };
            settings::add(s);
        }

        s = {};
        s.key = "mods.rescan";
        s.page = "Mods";
        s.section = "Folder";
        s.label = "Read the mods again";
        s.help = "After adding or changing files in a mod, or editing modloader.ini's priorities. A new mod folder shows here "
                 "after a restart.";
        s.type = Type::Action;
        s.button = "Rescan";
        s.onChange = [](int) {
            reloadPriorities();
            settings::refresh(); // the sliders follow the file
            g_dirty = true;
        };
        settings::add(s);

        s = {};
        s.key = "mods.open";
        s.page = "Mods";
        s.section = "Folder";
        s.label = "Mods folder";
        s.help = "Opens " + utf8(g_root) + " (made if it isn't there): put each mod in a folder of its own inside it.";
        s.type = Type::Action;
        s.button = "Open";
        s.onChange = [](int) {
            std::error_code e;
            fs::create_directories(g_root, e);
            platform::openFolder(utf8(fs::absolute(g_root, e)));
        };
        settings::add(s);
    }

    void onHostFrame()
    {
        if (!g_dirty)
            return;
        g_dirty = false;
        apply();
    }
}
