#include "Common.h"
#include "MemoryCard.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include "runtime/ps2_host_std.h"

#if defined(_WIN32) // [gta-recomp] file creation times (card directory order)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>    // AT_FDCWD
#include <sys/stat.h> // statx: the birth time
#endif

namespace ps2_stubs
{
    namespace
    {
        constexpr int32_t kMcCmdGetInfo = 0x01;
        constexpr int32_t kMcCmdOpen = 0x02;
        constexpr int32_t kMcCmdClose = 0x03;
        constexpr int32_t kMcCmdSeek = 0x04;
        constexpr int32_t kMcCmdRead = 0x05;
        constexpr int32_t kMcCmdWrite = 0x06;
        constexpr int32_t kMcCmdFlush = 0x0A;
        constexpr int32_t kMcCmdMkdir = 0x0B;
        constexpr int32_t kMcCmdChdir = 0x0C;
        constexpr int32_t kMcCmdGetDir = 0x0D;
        constexpr int32_t kMcCmdSetFileInfo = 0x0E;
        constexpr int32_t kMcCmdDelete = 0x0F;
        constexpr int32_t kMcCmdFormat = 0x10;
        constexpr int32_t kMcCmdUnformat = 0x11;
        constexpr int32_t kMcCmdGetEntSpace = 0x12;
        constexpr int32_t kMcCmdRename = 0x13;

        constexpr int32_t kMcResultSucceed = 0;
        constexpr int32_t kMcResultChangedCard = -1;
        constexpr int32_t kMcResultNoFormat = -2;
        constexpr int32_t kMcResultNoEntry = -4;
        constexpr int32_t kMcResultDeniedPermit = -5;
        constexpr int32_t kMcResultNotEmpty = -6;
        constexpr int32_t kMcResultUpLimitHandle = -7;

        constexpr int32_t kMcTypePs2 = 2;
        constexpr int32_t kMcFormatted = 1;
        constexpr int32_t kMcUnformatted = 0;
        constexpr int32_t kMcFreeClusters = 0x2000;
        constexpr size_t kMcMaxPathLen = 1024;
        constexpr size_t kMcMaxOpenFiles = 32;

        constexpr uint16_t kMcAttrReadable = 0x0001;
        constexpr uint16_t kMcAttrWriteable = 0x0002;
        constexpr uint16_t kMcAttrFile = 0x0010;
        constexpr uint16_t kMcAttrSubdir = 0x0020;
        constexpr uint16_t kMcAttrClosed = 0x0080;
        constexpr uint16_t kMcAttrExists = 0x8000;

        struct SceMcStDateTime
        {
            uint8_t Resv2 = 0;
            uint8_t Sec = 0;
            uint8_t Min = 0;
            uint8_t Hour = 0;
            uint8_t Day = 0;
            uint8_t Month = 0;
            uint16_t Year = 0;
        };

        struct SceMcTblGetDir
        {
            SceMcStDateTime _Create{};
            SceMcStDateTime _Modify{};
            uint32_t FileSizeByte = 0;
            uint16_t AttrFile = 0;
            uint16_t Reserve1 = 0;
            uint32_t Reserve2 = 0;
            uint32_t PdaAplNo = 0;
            char EntryName[32]{};
        };

        static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

        struct McOpenFile
        {
            FILE *file = nullptr;
            int32_t port = 0;
            std::filesystem::path hostPath;
        };

        struct McPortState
        {
            std::string currentDir = "/";
            bool formatted = true;
        };

        std::mutex g_mcStateMutex;
        int32_t g_mcNextFd = 1;
        int32_t g_mcLastCmd = 0;
        bool g_mcCommandPending = false;
        int32_t g_mcLastResult = 0;
        std::unordered_map<int32_t, McOpenFile> g_mcFiles;
        std::array<McPortState, 2> g_mcPorts{};
        int32_t g_cvMcFileCursor = 0;

        constexpr int32_t kCvMcSaveFileBytes = 0x838;
        constexpr int32_t kCvMcConfigFileBytes = 0x34;
        constexpr int32_t kCvMcIconInfoBytes = 0x3C4;
        constexpr int32_t kCvMcIconFileBytes = 0xB3F8;

        constexpr int32_t cvMcKilobytes(int32_t bytes)
        {
            return (bytes + 1023) / 1024;
        }

        constexpr int32_t kCvMcRequiredFreeKb =
            cvMcKilobytes(kCvMcSaveFileBytes) * 15 +
            cvMcKilobytes(kCvMcConfigFileBytes) +
            cvMcKilobytes(kCvMcIconInfoBytes) +
            cvMcKilobytes(kCvMcIconFileBytes) + 11;

        bool isValidMcPortSlot(int32_t port, int32_t slot)
        {
            return port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()) && slot == 0;
        }

        std::filesystem::path getMcRootPath(int32_t port)
        {
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            std::filesystem::path root = paths.mcRoot;
            if (root.empty())
            {
                if (!paths.elfDirectory.empty())
                {
                    root = paths.elfDirectory / "mc0";
                }
                else
                {
                    std::error_code ec;
                    const std::filesystem::path cwd = std::filesystem::current_path(ec);
                    root = ec ? std::filesystem::path("mc0") : (cwd / "mc0");
                }
            }

            root = root.lexically_normal();
            if (port <= 0)
            {
                return root;
            }

            const std::filesystem::path parent = root.parent_path();
            const std::string leaf = root.filename().string();
            const std::string lowerLeaf = toLowerAscii(leaf);
            if (lowerLeaf == "mc0")
            {
                return (parent / "mc1").lexically_normal();
            }
            if (leaf.empty())
            {
                return (root / "mc1").lexically_normal();
            }

            return (parent / (leaf + "_slot" + std::to_string(port))).lexically_normal();
        }

        void ensureMcRootExists(int32_t port)
        {
            std::error_code ec;
            std::filesystem::create_directories(getMcRootPath(port), ec);
        }

        std::vector<std::string> splitMcPathComponents(const std::string &value)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : value)
            {
                if (c == '/' || c == '\\')
                {
                    if (!current.empty())
                    {
                        parts.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }

            if (!current.empty())
            {
                parts.push_back(current);
            }

            return parts;
        }

        std::string joinMcPathComponents(const std::vector<std::string> &parts)
        {
            if (parts.empty())
            {
                return "/";
            }

            std::string joined = "/";
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i != 0u)
                {
                    joined.push_back('/');
                }
                joined.append(parts[i]);
            }
            return joined;
        }

        std::string normalizeGuestMcPathLocked(int32_t port, std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            const std::string lower = toLowerAscii(path);
            if (lower.rfind("mc0:", 0) == 0 || lower.rfind("mc1:", 0) == 0)
            {
                path = path.substr(4);
            }

            const bool absolute = !path.empty() && path.front() == '/';
            std::vector<std::string> parts;
            if (!absolute && port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()))
            {
                parts = splitMcPathComponents(g_mcPorts[static_cast<size_t>(port)].currentDir);
            }

            for (const std::string &part : splitMcPathComponents(path))
            {
                if (part.empty() || part == ".")
                {
                    continue;
                }

                if (part == "..")
                {
                    if (!parts.empty())
                    {
                        parts.pop_back();
                    }
                    continue;
                }

                parts.push_back(part);
            }

            return joinMcPathComponents(parts);
        }

        // [gta-recomp] Card entry names on the host. A card name may hold what a host file name cannot: VCS names a save
        // slot after the last mission passed, and "1 Kill Phil: Part 2" on NTFS became an empty file "1 Kill Phil" with
        // the save in an alternate data stream " Part 2" - the old slot deleted, the new one never found again
        // (game-internals.md 10f). Bytes that are not UTF-8 would throw in std::filesystem under the UTF-8 code page.
        // So characters Windows refuses, control bytes, bytes >= 0x7F, '%', trailing dots / spaces and device names are
        // stored as %XX, the same on every host (a card folder copied between systems stays readable); the guest sees
        // the name it wrote.
        std::string encodeMcName(const std::string &name)
        {
            static const char *const kHex = "0123456789ABCDEF";
            const size_t keep = name.find_last_not_of(". ") + 1; // trailing dots and spaces: Windows drops them
            std::string out;
            for (size_t i = 0; i < name.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(name[i]);
                if (c < 0x20u || c >= 0x7Fu || c == '%' || std::strchr("<>:\"/\\|?*", c) || i >= keep)
                {
                    out.push_back('%');
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0xFu]);
                }
                else
                {
                    out.push_back(static_cast<char>(c));
                }
            }
            // CON, PRN, AUX, NUL, COM1-9, LPT1-9, alone or before an extension
            std::string base = toLowerAscii(out.substr(0, out.find('.')));
            if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
                (base.size() == 4u && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) && base[3] >= '1' &&
                 base[3] <= '9'))
            {
                const unsigned char c = static_cast<unsigned char>(out[0]);
                out = std::string("%") + kHex[c >> 4] + kHex[c & 0xFu] + out.substr(1);
            }
            return out;
        }

        std::string decodeMcName(const std::string &name)
        {
            auto hex = [](char c) -> int
            {
                return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            };
            std::string out;
            for (size_t i = 0; i < name.size(); ++i)
            {
                if (name[i] == '%' && i + 2 < name.size() && hex(name[i + 1]) >= 0 && hex(name[i + 2]) >= 0)
                {
                    out.push_back(static_cast<char>(hex(name[i + 1]) * 16 + hex(name[i + 2])));
                    i += 2;
                }
                else
                {
                    out.push_back(name[i]);
                }
            }
            return out;
        }

        // A guest path ("/folder/name", normalised) on the host, each component encoded.
        std::filesystem::path guestMcPathToHostPath(int32_t port, const std::string &guestPath)
        {
            std::filesystem::path resolved = getMcRootPath(port);
            for (const std::string &part : splitMcPathComponents(guestPath))
            {
                const std::string host = encodeMcName(part);
#if !defined(_WIN32)
                // a name stored as it was before the encoding (Linux takes ':' and '?'): still found
                std::error_code ec;
                if (host != part && !std::filesystem::exists(resolved / host, ec) &&
                    std::filesystem::exists(resolved / part, ec))
                {
                    resolved /= part;
                    continue;
                }
#endif
                resolved /= host;
            }
            return resolved.lexically_normal();
        }

        // [gta-recomp] Card directory order. A real card lists a folder's entries in its directory-table order:
        // creation order, with a deleted entry's slot reused by the next file created there. Games depend on it - VCS
        // skips the first five entries of its save folder (".", "..", its data file, icon.sys, the icon) and looks
        // for its save slots after them - but a host folder has no such order, so each card folder keeps it in
        // kMcOrderFile (hidden from the guest): one name per line, an empty line for a freed slot. Entries it does
        // not know (copied in by hand, or written before it existed) go after the known ones, oldest first.
        constexpr const char *kMcOrderFile = ".ps2x-order";

        std::vector<std::string> readMcOrder(const std::filesystem::path &dir)
        {
            std::vector<std::string> names;
            std::ifstream in(dir / kMcOrderFile, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                names.push_back(line);
            }
            return names;
        }

        void writeMcOrder(const std::filesystem::path &dir, std::vector<std::string> names)
        {
            while (!names.empty() && names.back().empty())
            {
                names.pop_back();
            }
            std::ofstream out(dir / kMcOrderFile, std::ios::binary | std::ios::trunc);
            for (const std::string &name : names)
            {
                out << name << '\n';
            }
        }

        // Creation time where the host has one (Windows; Linux file systems with a birth time, ext4 / btrfs / xfs), else
        // the last write. Only an order between new entries: .ps2x-order holds the card's order once known.
        uint64_t mcCreationTime(const std::filesystem::directory_entry &entry)
        {
#if defined(_WIN32)
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &data))
            {
                return (static_cast<uint64_t>(data.ftCreationTime.dwHighDateTime) << 32) | data.ftCreationTime.dwLowDateTime;
            }
#elif defined(__linux__)
            struct statx st{};
            if (statx(AT_FDCWD, entry.path().c_str(), 0, STATX_BTIME | STATX_MTIME, &st) == 0)
            {
                const statx_timestamp &t = (st.stx_mask & STATX_BTIME) ? st.stx_btime : st.stx_mtime;
                return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + t.tv_nsec;
            }
#endif
            std::error_code ec;
            return static_cast<uint64_t>(entry.last_write_time(ec).time_since_epoch().count());
        }

#if defined(_WIN32)
        // [gta-recomp] Saves lost to an NTFS stream before the names were encoded (encodeMcName): an empty file whose
        // named stream holds the data ("1 Kill Phil" with " Part 2") becomes the file it should have been
        // ("1 Kill Phil%3A Part 2"), taking the empty file's place in the card order.
        void recoverMcStreams(const std::filesystem::path &dir)
        {
            std::error_code ec;
            std::vector<std::filesystem::path> empty;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                std::error_code entryEc;
                if (entry.is_regular_file(entryEc) && entry.file_size(entryEc) == 0u && !entryEc)
                {
                    empty.push_back(entry.path());
                }
            }
            for (const std::filesystem::path &file : empty)
            {
                try
                {
                    std::vector<std::pair<std::wstring, uint64_t>> streams; // name, size
                    WIN32_FIND_STREAM_DATA data{};
                    HANDLE find = FindFirstStreamW(file.c_str(), FindStreamInfoStandard, &data, 0);
                    if (find == INVALID_HANDLE_VALUE)
                    {
                        continue;
                    }
                    do
                    {
                        // ":name:$DATA"; the file's own data is "::$DATA". Small streams are Windows' own (a download's
                        // Zone.Identifier); the game's smallest file is icon.sys, 964 bytes
                        const std::wstring s = data.cStreamName;
                        if (data.StreamSize.QuadPart >= 900 && s.size() > 7u && s.compare(s.size() - 6u, 6u, L":$DATA") == 0)
                        {
                            streams.emplace_back(s.substr(1u, s.size() - 7u), static_cast<uint64_t>(data.StreamSize.QuadPart));
                        }
                    } while (FindNextStreamW(find, &data));
                    FindClose(find);

                    std::string recovered;
                    size_t moved = 0;
                    // A save slot ("<1-8> <mission>", game-internals.md 10c) already holding a save - the player saved
                    // again after the loss, maybe a new game - shows only the newer of the two: the recovered one goes
                    // to the first free slot instead (the game takes the slot from the name alone; 10f)
                    auto slotTaken = [&](char digit)
                    {
                        std::error_code listEc;
                        for (const auto &entry : std::filesystem::directory_iterator(dir, listEc))
                        {
                            std::error_code entryEc;
                            const std::string name = decodeMcName(entry.path().filename().string());
                            if (name.size() > 2u && name[0] == digit && name[1] == ' ' &&
                                entry.is_regular_file(entryEc) && entry.file_size(entryEc) > 0u && !entryEc)
                            {
                                return true;
                            }
                        }
                        return false;
                    };
                    for (const auto &[stream, size] : streams)
                    {
                        const std::filesystem::path source(file.native() + L":" + stream);
                        std::string guest =
                            decodeMcName(file.filename().string()) + ":" + std::filesystem::path(stream).string();
                        if (guest.size() > 2u && guest[0] >= '1' && guest[0] <= '8' && guest[1] == ' ' && slotTaken(guest[0]))
                        {
                            for (char digit = '1'; digit <= '8'; ++digit)
                            {
                                if (!slotTaken(digit))
                                {
                                    std::fprintf(stderr, "[MC] slot %c holds a newer save: the recovered one goes to slot %c\n",
                                                 guest[0], digit);
                                    guest[0] = digit;
                                    break;
                                }
                            }
                        }
                        const std::filesystem::path target = dir / encodeMcName(guest);
                        if (std::filesystem::exists(target, ec))
                        {
                            continue;
                        }
                        {
                            std::ifstream in(source, std::ios::binary);
                            std::ofstream out(target, std::ios::binary);
                            out << in.rdbuf();
                        }
                        if (std::filesystem::file_size(target, ec) != size || ec)
                        {
                            std::filesystem::remove(target, ec);
                            continue;
                        }
                        std::filesystem::last_write_time(target, std::filesystem::last_write_time(file, ec), ec);
                        DeleteFileW(source.c_str());
                        recovered = target.filename().string();
                        ++moved;
                        std::fprintf(stderr, "[MC] recovered the save '%s' from an NTFS stream of '%s'\n", guest.c_str(),
                                     file.filename().string().c_str());
                    }
                    if (!recovered.empty() && moved == streams.size() && std::filesystem::remove(file, ec))
                    {
                        std::vector<std::string> order = readMcOrder(dir);
                        auto it = std::find(order.begin(), order.end(), file.filename().string());
                        if (it != order.end())
                        {
                            *it = recovered;
                            writeMcOrder(dir, order);
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[MC] stream recovery of a file failed: %s\n", e.what());
                }
            }
        }
#endif

        // A card folder's entries (without "." / "..") in card order; records entries it did not know yet.
        std::vector<std::filesystem::directory_entry> listMcDirOrdered(const std::filesystem::path &dir)
        {
#if defined(_WIN32)
            recoverMcStreams(dir); // [gta-recomp]
#endif
            std::vector<std::filesystem::directory_entry> onDisk;
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied, ec))
            {
                if (ec)
                {
                    break;
                }
                if (entry.path().filename() != kMcOrderFile)
                {
                    onDisk.push_back(entry);
                }
            }

            std::vector<std::string> order = readMcOrder(dir);
            const size_t knownCount = order.size();
            std::vector<std::filesystem::directory_entry> result;
            std::vector<bool> placed(onDisk.size(), false);
            bool changed = false;
            for (std::string &name : order)
            {
                if (name.empty())
                {
                    continue;
                }
                bool found = false;
                for (size_t i = 0; i < onDisk.size(); ++i)
                {
                    if (!placed[i] && onDisk[i].path().filename().string() == name)
                    {
                        placed[i] = true;
                        result.push_back(onDisk[i]);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    name.clear(); // removed behind the card's back: its slot is free
                    changed = true;
                }
            }

            std::vector<std::filesystem::directory_entry> unknown;
            for (size_t i = 0; i < onDisk.size(); ++i)
            {
                if (!placed[i])
                {
                    unknown.push_back(onDisk[i]);
                }
            }
            std::sort(unknown.begin(), unknown.end(),
                      [](const std::filesystem::directory_entry &lhs, const std::filesystem::directory_entry &rhs)
                      {
                          const uint64_t a = mcCreationTime(lhs), b = mcCreationTime(rhs);
                          return a != b ? a < b : lhs.path().filename().string() < rhs.path().filename().string();
                      });
            for (const auto &entry : unknown)
            {
                result.push_back(entry);
                order.push_back(entry.path().filename().string());
                changed = true;
            }
            if (changed || order.size() != knownCount)
            {
                writeMcOrder(dir, order);
            }
            return result;
        }

        // Before a new entry is created: it takes the folder's first free slot, or goes last.
        void reserveMcOrderSlot(const std::filesystem::path &hostPath)
        {
            const std::filesystem::path dir = hostPath.parent_path();
            listMcDirOrdered(dir); // records what is there
            std::vector<std::string> order = readMcOrder(dir);
            const std::string name = hostPath.filename().string();
            if (std::find(order.begin(), order.end(), name) != order.end())
            {
                return;
            }
            auto slot = std::find(order.begin(), order.end(), std::string{});
            if (slot != order.end())
            {
                *slot = name;
            }
            else
            {
                order.push_back(name);
            }
            writeMcOrder(dir, order);
        }

        // After an entry was deleted (from) or renamed (from -> to, same folder): its slot is freed or keeps the new name.
        void updateMcOrderSlot(const std::filesystem::path &from, const std::string &to)
        {
            const std::filesystem::path dir = from.parent_path();
            std::vector<std::string> order = readMcOrder(dir);
            auto it = std::find(order.begin(), order.end(), from.filename().string());
            if (it == order.end())
            {
                return;
            }
            *it = to;
            writeMcOrder(dir, order);
        }

        // An empty card folder may still hold its order file.
        bool isMcDirEmpty(const std::filesystem::path &dir)
        {
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (entry.path().filename() != kMcOrderFile)
                {
                    return false;
                }
            }
            return true;
        }

        bool localtimeSafeMc(const std::time_t *value, std::tm *out)
        {
#ifdef _WIN32
            return localtime_s(out, value) == 0;
#else
            return localtime_r(value, out) != nullptr;
#endif
        }

        std::time_t fileTimeToTimeTMc(std::filesystem::file_time_type value)
        {
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(systemTime);
        }

        void writeMcCString(uint8_t *rdram, uint32_t addr, const std::string &value)
        {
            if (addr == 0u)
            {
                return;
            }

            uint8_t *dst = getMemPtr(rdram, addr);
            if (!dst)
            {
                return;
            }

            std::memcpy(dst, value.c_str(), value.size() + 1u);
        }

        void writeMcDateTime(SceMcStDateTime &out, std::time_t value)
        {
            std::tm tm{};
            if (!localtimeSafeMc(&value, &tm))
            {
                std::memset(&out, 0, sizeof(out));
                return;
            }

            out.Resv2 = 0;
            out.Sec = static_cast<uint8_t>(tm.tm_sec);
            out.Min = static_cast<uint8_t>(tm.tm_min);
            out.Hour = static_cast<uint8_t>(tm.tm_hour);
            out.Day = static_cast<uint8_t>(tm.tm_mday);
            out.Month = static_cast<uint8_t>(tm.tm_mon + 1);
            out.Year = static_cast<uint16_t>(tm.tm_year + 1900);
        }

        void fillMcDirTableEntry(SceMcTblGetDir &entry,
                                 const std::string &name,
                                 bool isDirectory,
                                 uint32_t sizeBytes,
                                 std::time_t modifiedTime)
        {
            std::memset(&entry, 0, sizeof(entry));
            writeMcDateTime(entry._Create, modifiedTime);
            writeMcDateTime(entry._Modify, modifiedTime);
            entry.FileSizeByte = isDirectory ? 0u : sizeBytes;
            entry.AttrFile = static_cast<uint16_t>(kMcAttrReadable |
                                                   kMcAttrWriteable |
                                                   (isDirectory ? kMcAttrSubdir : kMcAttrFile) |
                                                   kMcAttrClosed |
                                                   kMcAttrExists);
            std::strncpy(entry.EntryName, name.c_str(), sizeof(entry.EntryName) - 1u);
            entry.EntryName[sizeof(entry.EntryName) - 1u] = '\0';
        }

        bool wildcardMatch(const std::string &pattern, const std::string &value)
        {
            size_t patternPos = 0u;
            size_t valuePos = 0u;
            size_t starPos = std::string::npos;
            size_t matchPos = 0u;

            while (valuePos < value.size())
            {
                if (patternPos < pattern.size() &&
                    (pattern[patternPos] == '?' || pattern[patternPos] == value[valuePos]))
                {
                    ++patternPos;
                    ++valuePos;
                }
                else if (patternPos < pattern.size() && pattern[patternPos] == '*')
                {
                    starPos = patternPos++;
                    matchPos = valuePos;
                }
                else if (starPos != std::string::npos)
                {
                    patternPos = starPos + 1u;
                    valuePos = ++matchPos;
                }
                else
                {
                    return false;
                }
            }

            while (patternPos < pattern.size() && pattern[patternPos] == '*')
            {
                ++patternPos;
            }

            return patternPos == pattern.size();
        }

        void setMcCommandResultLocked(int32_t cmd, int32_t result)
        {
            g_mcLastCmd = cmd;
            g_mcLastResult = result;
            g_mcCommandPending = true;
        }

        void closeMcFilesLocked()
        {
            for (auto &[fd, openFile] : g_mcFiles)
            {
                if (openFile.file)
                {
                    std::fclose(openFile.file);
                    openFile.file = nullptr;
                }
            }
            g_mcFiles.clear();
        }

        void closeMcFilesForPortLocked(int32_t port)
        {
            for (auto it = g_mcFiles.begin(); it != g_mcFiles.end();)
            {
                if (it->second.port == port)
                {
                    if (it->second.file)
                    {
                        std::fclose(it->second.file);
                    }
                    it = g_mcFiles.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        int32_t allocateMcFdLocked(FILE *file, int32_t port, const std::filesystem::path &hostPath)
        {
            if (!file)
            {
                return kMcResultDeniedPermit;
            }
            if (g_mcFiles.size() >= kMcMaxOpenFiles)
            {
                return kMcResultUpLimitHandle;
            }

            for (int attempt = 0; attempt < 0x10000; ++attempt)
            {
                if (g_mcNextFd <= 0)
                {
                    g_mcNextFd = 1;
                }

                const int32_t fd = g_mcNextFd++;
                if (g_mcFiles.find(fd) != g_mcFiles.end())
                {
                    continue;
                }

                g_mcFiles.emplace(fd, McOpenFile{file, port, hostPath});
                return fd;
            }

            return kMcResultUpLimitHandle;
        }

        FILE *openMcHostFile(const std::filesystem::path &hostPath, uint32_t flags)
        {
            const uint32_t access = flags & PS2_FIO_O_RDWR;
            const bool read = (access == PS2_FIO_O_RDONLY) || (access == PS2_FIO_O_RDWR);
            const bool write = (access == PS2_FIO_O_WRONLY) || (access == PS2_FIO_O_RDWR);
            const bool append = (flags & PS2_FIO_O_APPEND) != 0u;
            const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
            const bool truncate = (flags & PS2_FIO_O_TRUNC) != 0u;

            std::error_code ec;
            const bool exists = std::filesystem::exists(hostPath, ec) && !ec;

            const char *mode = "rb";
            if (read && write)
            {
                if (append)
                {
                    mode = exists ? "a+b" : "w+b";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "w+b";
                }
                else
                {
                    mode = "r+b";
                }
            }
            else if (write)
            {
                if (append)
                {
                    mode = exists ? "ab" : "wb";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "wb";
                }
                else
                {
                    mode = "r+b";
                }
            }

            return std::fopen(hostPath.string().c_str(), mode);
        }

        // [gta-recomp] Automatic card backups. The game deletes a slot's file before it writes the new one, and when a
        // write fails it deletes its whole save folder (game-internals.md 10f): a crash or a host error in between loses
        // saves. Before the first change of a burst (a delete, a write to an existing file, a rename, a format) both
        // cards go into <data>/backups/auto-<date>.zip in tools/backup.py's format (the launcher lists and restores
        // them); the newest kMcAutoBackups are kept. PS2X_MC_BACKUP=0 turns it off.
        constexpr size_t kMcAutoBackups = 10;
        constexpr auto kMcBackupQuiet = std::chrono::seconds(10); // one save's deletes and writes take a second or two

        uint32_t crc32Mc(const std::vector<uint8_t> &data)
        {
            static const std::array<uint32_t, 256> table = []
            {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                    {
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    }
                    t[i] = c;
                }
                return t;
            }();
            uint32_t crc = 0xFFFFFFFFu;
            for (uint8_t b : data)
            {
                crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
            }
            return crc ^ 0xFFFFFFFFu;
        }

        // Creation and modification times in seconds since 1970 (backup.py's manifest).
        std::pair<double, double> mcEpochTimes(const std::filesystem::path &path)
        {
#if defined(_WIN32)
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                auto seconds = [](const FILETIME &ft)
                {
                    const uint64_t v = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                    return (static_cast<double>(v) - 116444736000000000.0) / 1e7;
                };
                return {seconds(data.ftCreationTime), seconds(data.ftLastWriteTime)};
            }
#elif defined(__linux__)
            struct statx st{};
            if (statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME | STATX_MTIME, &st) == 0)
            {
                const double m = static_cast<double>(st.stx_mtime.tv_sec) + st.stx_mtime.tv_nsec / 1e9;
                const double b = (st.stx_mask & STATX_BTIME) ? static_cast<double>(st.stx_btime.tv_sec) + st.stx_btime.tv_nsec / 1e9 : m;
                return {b, m};
            }
#endif
            std::error_code ec;
            const double m = static_cast<double>(fileTimeToTimeTMc(std::filesystem::last_write_time(path, ec)));
            return {m, m};
        }

        std::string jsonStringMc(const std::string &value)
        {
            std::string out = "\"";
            for (const char ch : value)
            {
                const unsigned char c = static_cast<unsigned char>(ch);
                if (c == '"' || c == '\\')
                {
                    out.push_back('\\');
                    out.push_back(ch);
                }
                else if (c < 0x20u)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out.push_back(ch);
                }
            }
            return out + "\"";
        }

        // A zip of stored (uncompressed) entries: names (a trailing '/' for a folder) and their data.
        bool writeZipMc(const std::filesystem::path &out, const std::vector<std::pair<std::string, std::vector<uint8_t>>> &files)
        {
            std::vector<uint8_t> zip, central;
            auto put16 = [](std::vector<uint8_t> &v, uint32_t x)
            {
                v.push_back(static_cast<uint8_t>(x));
                v.push_back(static_cast<uint8_t>(x >> 8));
            };
            auto put32 = [&](std::vector<uint8_t> &v, uint32_t x)
            {
                put16(v, x & 0xFFFFu);
                put16(v, x >> 16);
            };
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtimeSafeMc(&now, &tm);
            const uint32_t dosTime = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
            const uint32_t dosDate = ((std::max(tm.tm_year, 80) - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
            for (const auto &[name, data] : files)
            {
                const uint32_t offset = static_cast<uint32_t>(zip.size()), crc = crc32Mc(data);
                const uint32_t size = static_cast<uint32_t>(data.size());
                auto header = [&](std::vector<uint8_t> &v, bool isCentral)
                {
                    put32(v, isCentral ? 0x02014B50u : 0x04034B50u);
                    if (isCentral)
                    {
                        put16(v, 20); // made by
                    }
                    put16(v, 20);     // needed
                    put16(v, 0x800);  // UTF-8 names
                    put16(v, 0);      // stored
                    put16(v, dosTime);
                    put16(v, dosDate);
                    put32(v, crc);
                    put32(v, size);
                    put32(v, size);
                    put16(v, static_cast<uint32_t>(name.size()));
                    put16(v, 0);      // extra
                    if (isCentral)
                    {
                        put16(v, 0);  // comment
                        put16(v, 0);  // disk
                        put16(v, 0);  // internal attributes
                        put32(v, !name.empty() && name.back() == '/' ? 0x10u : 0u);
                        put32(v, offset);
                    }
                    v.insert(v.end(), name.begin(), name.end());
                };
                header(zip, false);
                zip.insert(zip.end(), data.begin(), data.end());
                header(central, true);
            }
            const uint32_t centralOffset = static_cast<uint32_t>(zip.size());
            zip.insert(zip.end(), central.begin(), central.end());
            put32(zip, 0x06054B50u);
            put16(zip, 0);
            put16(zip, 0);
            put16(zip, static_cast<uint32_t>(files.size()));
            put16(zip, static_cast<uint32_t>(files.size()));
            put32(zip, static_cast<uint32_t>(central.size()));
            put32(zip, centralOffset);
            put16(zip, 0);

            const std::filesystem::path tmp = out.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char *>(zip.data()), static_cast<std::streamsize>(zip.size()));
                if (!f)
                {
                    return false;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, out, ec);
            return !ec;
        }

        void backupMcCardsLocked()
        {
            static std::chrono::steady_clock::time_point s_last{};
            static bool s_any = false;
            const char *env = std::getenv("PS2X_MC_BACKUP");
            const auto now = ps2x::hostNow();
            if ((env && env[0] == '0') || (s_any && now - s_last < kMcBackupQuiet))
            {
                return;
            }
            s_any = true;
            s_last = now;

            try
            {
                const std::filesystem::path data = getMcRootPath(0).parent_path();
                struct Entry
                {
                    std::string rel;
                    bool dir;
                    double ctime, mtime;
                    std::filesystem::path full;
                };
                std::vector<Entry> entries;
                size_t fileCount = 0;
                for (int32_t port = 0; port < static_cast<int32_t>(g_mcPorts.size()); ++port)
                {
                    const std::filesystem::path card = getMcRootPath(port);
                    std::error_code ec;
                    if (!std::filesystem::is_directory(card, ec))
                    {
                        continue;
                    }
                    for (auto it = std::filesystem::recursive_directory_iterator(card, ec);
                         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                    {
                        std::error_code entryEc;
                        const bool dir = it->is_directory(entryEc);
                        const auto [c, m] = mcEpochTimes(it->path());
                        entries.push_back({std::filesystem::relative(it->path(), data, entryEc).generic_string(), dir, c, m,
                                           it->path()});
                        fileCount += dir ? 0u : 1u;
                    }
                    const auto [c, m] = mcEpochTimes(card);
                    entries.push_back({std::filesystem::relative(card, data, ec).generic_string(), true, c, m, card});
                }
                if (fileCount == 0u)
                {
                    return; // nothing saved yet
                }
                // creation order, as backup.py: a restore makes folders and files again in that order
                std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b)
                          {
                              const auto depth = [](const std::string &s) { return std::count(s.begin(), s.end(), '/'); };
                              if (a.ctime != b.ctime)
                                  return a.ctime < b.ctime;
                              if (depth(a.rel) != depth(b.rel))
                                  return depth(a.rel) < depth(b.rel);
                              return a.rel < b.rel;
                          });

                std::ostringstream manifest;
                manifest.precision(17);
                manifest << "{\"created\": " << static_cast<long long>(std::time(nullptr))
                         << ", \"source\": " << jsonStringMc(data.string()) << ", \"auto\": true, \"entries\": [";
                std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
                files.emplace_back("backup.json", std::vector<uint8_t>{});
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    const Entry &e = entries[i];
                    manifest << (i ? ", " : "") << "{\"path\": " << jsonStringMc(e.rel) << ", \"dir\": "
                             << (e.dir ? "true" : "false") << ", \"ctime\": " << e.ctime << ", \"mtime\": " << e.mtime << "}";
                    if (e.dir)
                    {
                        files.emplace_back(e.rel + "/", std::vector<uint8_t>{});
                        continue;
                    }
                    std::ifstream in(e.full, std::ios::binary);
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    files.emplace_back(e.rel, std::move(bytes));
                }
                manifest << "]}";
                const std::string text = manifest.str();
                files[0].second.assign(text.begin(), text.end());

                const std::filesystem::path dir = data / "backups";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const std::time_t wall = std::time(nullptr);
                std::tm tm{};
                localtimeSafeMc(&wall, &tm);
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "auto-%Y%m%d-%H%M%S", &tm);
                std::filesystem::path out = dir / (std::string(stamp) + ".zip");
                for (int n = 2; std::filesystem::exists(out, ec); ++n)
                {
                    out = dir / (std::string(stamp) + "-" + std::to_string(n) + ".zip");
                }
                if (!writeZipMc(out, files))
                {
                    std::fprintf(stderr, "[MC] card backup to %s failed\n", out.string().c_str());
                    return;
                }
                std::fprintf(stderr, "[MC] card backup before a change: %s (%zu files)\n", out.string().c_str(), fileCount);

                std::vector<std::filesystem::path> autos;
                for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
                {
                    const std::string name = entry.path().filename().string();
                    if (name.rfind("auto-", 0) == 0 && entry.path().extension() == ".zip")
                    {
                        autos.push_back(entry.path());
                    }
                }
                std::sort(autos.begin(), autos.end()); // the names sort by time
                for (size_t i = 0; i + kMcAutoBackups < autos.size(); ++i)
                {
                    std::filesystem::remove(autos[i], ec);
                }
            }
            catch (const std::exception &e)
            {
                std::fprintf(stderr, "[MC] card backup failed: %s\n", e.what());
            }
        }
    }

    MemoryCardDebugSnapshot getMemoryCardDebugSnapshot()
    {
        MemoryCardDebugSnapshot snapshot{};
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        snapshot.nextFd = g_mcNextFd;
        snapshot.lastCmd = g_mcLastCmd;
        snapshot.lastResult = g_mcLastResult;
        snapshot.cvFileCursor = g_cvMcFileCursor;

        for (size_t i = 0; i < g_mcPorts.size(); ++i)
        {
            snapshot.ports[i].port = static_cast<int32_t>(i);
            snapshot.ports[i].currentDir = g_mcPorts[i].currentDir;
            snapshot.ports[i].formatted = g_mcPorts[i].formatted;
            snapshot.ports[i].rootPath = getMcRootPath(static_cast<int32_t>(i));
        }

        snapshot.openFiles.reserve(g_mcFiles.size());
        for (const auto &[fd, file] : g_mcFiles)
        {
            MemoryCardDebugOpenFile row{};
            row.fd = fd;
            row.port = file.port;
            row.hostPath = file.hostPath;
            snapshot.openFiles.push_back(std::move(row));
        }
        std::sort(snapshot.openFiles.begin(), snapshot.openFiles.end(), [](const MemoryCardDebugOpenFile &a, const MemoryCardDebugOpenFile &b)
                  { return a.fd < b.fd; });
        return snapshot;
    }

    void sceMcChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t pathAddr = getRegU32(ctx, 6);
        const uint32_t currentDirAddr = getRegU32(ctx, 7);
        const std::string requestedDir =
            (pathAddr != 0u) ? readPs2CStringBounded(rdram, pathAddr, kMcMaxPathLen) : std::string{};

        std::string currentDir = "/";
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                currentDir = state.currentDir;
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string resolvedDir =
                        requestedDir.empty() ? state.currentDir : normalizeGuestMcPathLocked(port, requestedDir);
                    const std::filesystem::path hostDir = guestMcPathToHostPath(port, resolvedDir);
                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        state.currentDir = resolvedDir;
                        currentDir = resolvedDir;
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdChdir, result);
        }

        RUNTIME_LOG("[MC] Chdir port=" << port << " '" << requestedDir
                                       << "' -> result=" << result << " cwd='" << currentDir << "'");
        writeMcCString(rdram, currentDirAddr, currentDir);
        setReturnS32(ctx, 0);
    }

    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end())
            {
                if (!it->second.file || std::fclose(it->second.file) == 0)
                {
                    result = kMcResultSucceed;
                }
                g_mcFiles.erase(it);
            }
            setMcCommandResultLocked(kMcCmdClose, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    if (guestPath != "/")
                    {
                        const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                        std::error_code ec;
                        if (std::filesystem::exists(hostPath, ec) && !ec)
                        {
                            const bool isDir = std::filesystem::is_directory(hostPath, ec);
                            if (!isDir)
                            {
                                backupMcCardsLocked(); // [gta-recomp]
                            }
                            if (isDir && !isMcDirEmpty(hostPath))
                            {
                                result = kMcResultNotEmpty;
                            }
                            else if ((!isDir || (std::filesystem::remove(hostPath / kMcOrderFile, ec), true)) &&
                                     std::filesystem::remove(hostPath, ec) && !ec)
                            {
                                updateMcOrderSlot(hostPath, {}); // [gta-recomp] slot freed
                                result = kMcResultSucceed;
                            }
                            else
                            {
                                result = kMcResultDeniedPermit;
                            }
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdDelete, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
            }
        }

        // libmc teardown is just a local state reset in this immediate runtime model.
        setReturnS32(ctx, 0);
    }

    void sceMcFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                result = (std::fflush(it->second.file) == 0) ? kMcResultSucceed : kMcResultDeniedPermit;
            }
            setMcCommandResultLocked(kMcCmdFlush, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcFormat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                backupMcCardsLocked(); // [gta-recomp]
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = true;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdFormat, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string rawPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const int32_t maxEntries = static_cast<int32_t>(getRegU32(ctx, 8));
        const uint32_t tableAddr = getRegU32(ctx, 9);

        std::vector<SceMcTblGetDir> entries;
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestQuery =
                        normalizeGuestMcPathLocked(port, rawPath.empty() ? "." : rawPath);
                    const bool hasWildcard =
                        guestQuery.find('*') != std::string::npos || guestQuery.find('?') != std::string::npos;

                    // [gta-recomp] the folder and the pattern as guest strings (card names are not host paths:
                    // encodeMcName), the listed host names decoded back
                    const size_t lastSlash = guestQuery.find_last_of('/');
                    std::string parentGuest = guestQuery.substr(0, lastSlash == 0u ? 1u : lastSlash);
                    std::string pattern = guestQuery.substr(lastSlash + 1u);
                    if (!hasWildcard)
                    {
                        const std::filesystem::path queryHostPath = guestMcPathToHostPath(port, guestQuery);
                        std::error_code queryEc;
                        if (std::filesystem::exists(queryHostPath, queryEc) && !queryEc &&
                            std::filesystem::is_directory(queryHostPath, queryEc))
                        {
                            parentGuest = guestQuery;
                            pattern = "*";
                        }
                    }

                    if (pattern.empty())
                    {
                        pattern = "*";
                    }

                    const std::filesystem::path hostDir = guestMcPathToHostPath(port, parentGuest);

                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        const std::time_t now = std::time(nullptr);
                        auto appendSpecial = [&](const std::string &name)
                        {
                            if (!wildcardMatch(pattern, name))
                            {
                                return;
                            }
                            SceMcTblGetDir entry{};
                            fillMcDirTableEntry(entry, name, true, 0u, now);
                            entries.push_back(entry);
                        };

                        appendSpecial(".");
                        appendSpecial("..");

                        // [gta-recomp] in card order (see kMcOrderFile), not sorted by name
                        const std::vector<std::filesystem::directory_entry> dirEntries = listMcDirOrdered(hostDir);

                        for (const auto &entry : dirEntries)
                        {
                            const std::string name = decodeMcName(entry.path().filename().string());
                            if (!wildcardMatch(pattern, name))
                            {
                                continue;
                            }

                            std::error_code entryEc;
                            const bool isDirectory = entry.is_directory(entryEc) && !entryEc;
                            const uint32_t sizeBytes =
                                isDirectory ? 0u : static_cast<uint32_t>(entry.file_size(entryEc));
                            entryEc.clear();
                            const std::time_t modifiedTime = fileTimeToTimeTMc(entry.last_write_time(entryEc));
                            SceMcTblGetDir tableEntry{};
                            fillMcDirTableEntry(tableEntry,
                                                name,
                                                isDirectory,
                                                sizeBytes,
                                                entryEc ? now : modifiedTime);
                            entries.push_back(tableEntry);
                        }

                        const size_t entryCount =
                            std::min(entries.size(), maxEntries > 0 ? static_cast<size_t>(maxEntries) : 0u);
                        if (entryCount == 0u || tableAddr == 0u)
                        {
                            result = static_cast<int32_t>(entryCount);
                        }
                        else if (uint8_t *dst = getMemPtr(rdram, tableAddr))
                        {
                            std::memcpy(dst, entries.data(), entryCount * sizeof(SceMcTblGetDir));
                            result = static_cast<int32_t>(entryCount);
                        }
                        else
                        {
                            result = kMcResultDeniedPermit;
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdGetDir, result);
        }
        RUNTIME_LOG("[MC] GetDir port=" << port << " '" << rawPath
                                        << "' maxent=" << maxEntries << " -> result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetEntSpace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            setMcCommandResultLocked(kMcCmdGetEntSpace, 1024);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t typePtr = getRegU32(ctx, 6);
        const uint32_t freePtr = getRegU32(ctx, 7);
        const uint32_t formatPtr = getRegU32(ctx, 8);

        int32_t cardType = 0;
        int32_t freeBlocks = 0;
        int32_t format = kMcUnformatted;
        int32_t result = kMcResultNoEntry;

        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                cardType = kMcTypePs2;
                freeBlocks = state.formatted ? kMcFreeClusters : 0;
                format = state.formatted ? kMcFormatted : kMcUnformatted;
                result = state.formatted ? kMcResultSucceed : kMcResultNoFormat;
            }

            setMcCommandResultLocked(kMcCmdGetInfo, result);
        }

        if (typePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, typePtr))
            {
                std::memcpy(out, &cardType, sizeof(cardType));
            }
        }
        if (freePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, freePtr))
            {
                std::memcpy(out, &freeBlocks, sizeof(freeBlocks));
            }
        }
        if (formatPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, formatPtr))
            {
                std::memcpy(out, &format, sizeof(format));
            }
        }

        RUNTIME_LOG("[MC] GetInfo port=" << port << " type=" << cardType
                                         << " free=" << freeBlocks << " format=" << format
                                         << " result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
                state.formatted = true;
            }
        }
        ensureMcRootExists(0);
        ensureMcRootExists(1);
        setReturnS32(ctx, 0);
    }

    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = std::filesystem::is_directory(hostPath, ec) ? kMcResultSucceed : kMcResultDeniedPermit;
                    }
                    else if (std::filesystem::exists(hostPath.parent_path(), ec) && !ec &&
                             (reserveMcOrderSlot(hostPath), std::filesystem::create_directory(hostPath, ec)) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                    else
                    {
                        result = std::filesystem::exists(hostPath.parent_path(), ec) && !ec
                                     ? kMcResultDeniedPermit
                                     : kMcResultNoEntry;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdMkdir, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const uint32_t flags = getRegU32(ctx, 7);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
                    const bool exists = std::filesystem::exists(hostPath, ec) && !ec;
                    if (guestPath == "/")
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (exists && std::filesystem::is_directory(hostPath, ec))
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (!exists && !create)
                    {
                        result = kMcResultNoEntry;
                    }
                    else if (!std::filesystem::exists(hostPath.parent_path(), ec) || ec)
                    {
                        result = kMcResultNoEntry;
                    }
                    else
                    {
                        if (!exists)
                        {
                            reserveMcOrderSlot(hostPath); // [gta-recomp] card directory order
                        }
                        else if ((flags & PS2_FIO_O_RDWR) != PS2_FIO_O_RDONLY)
                        {
                            backupMcCardsLocked(); // [gta-recomp] an existing file about to change
                        }
                        FILE *file = openMcHostFile(hostPath, flags);
                        if (!file)
                        {
                            result = exists ? kMcResultDeniedPermit : kMcResultNoEntry;
                        }
                        else
                        {
                            result = allocateMcFdLocked(file, port, hostPath);
                            if (result < 0)
                            {
                                std::fclose(file);
                            }
                        }
                    }
                }
            }
            setMcCommandResultLocked(kMcCmdOpen, result);
        }
        RUNTIME_LOG("[MC] Open port=" << port << " '" << path << "' flags=0x" << std::hex << flags << std::dec
                                      << " -> " << result); // [gta-recomp] MC trace
        setReturnS32(ctx, 0);
    }

    void sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t dstAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        uint8_t *dst = (size > 0) ? getMemPtr(rdram, dstAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!dst)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesRead = std::fread(dst, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesRead);
                if (std::ferror(it->second.file))
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdRead, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string oldPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const std::string newPath = readPs2CStringBounded(rdram, getRegU32(ctx, 7), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path oldHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, oldPath));
                    const std::filesystem::path newHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, newPath));
                    std::error_code ec;
                    if (std::filesystem::exists(oldHostPath, ec) && !ec &&
                        std::filesystem::exists(newHostPath.parent_path(), ec) && !ec)
                    {
                        backupMcCardsLocked(); // [gta-recomp]
                        std::filesystem::rename(oldHostPath, newHostPath, ec);
                        result = ec ? kMcResultDeniedPermit : kMcResultSucceed;
                        if (!ec) // [gta-recomp] card directory order
                        {
                            if (oldHostPath.parent_path() == newHostPath.parent_path())
                            {
                                updateMcOrderSlot(oldHostPath, newHostPath.filename().string());
                            }
                            else
                            {
                                updateMcOrderSlot(oldHostPath, {});
                                reserveMcOrderSlot(newHostPath);
                            }
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdRename, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t offset = static_cast<int32_t>(getRegU32(ctx, 5));
        const int32_t origin = static_cast<int32_t>(getRegU32(ctx, 6));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                int whence = SEEK_SET;
                if (origin == PS2_FIO_SEEK_CUR)
                {
                    whence = SEEK_CUR;
                }
                else if (origin == PS2_FIO_SEEK_END)
                {
                    whence = SEEK_END;
                }

                if (std::fseek(it->second.file, offset, whence) == 0)
                {
                    const long position = std::ftell(it->second.file);
                    result = (position >= 0) ? static_cast<int32_t>(position) : kMcResultDeniedPermit;
                }
                else
                {
                    result = kMcResultDeniedPermit;
                }
            }

            setMcCommandResultLocked(kMcCmdSeek, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path hostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, path));
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdSetFileInfo, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cmdPtr = getRegU32(ctx, 5);
        const uint32_t resultPtr = getRegU32(ctx, 6);
        int32_t cmd = 0;
        int32_t result = 0;
        bool hadPending = false;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            hadPending = g_mcCommandPending;
            g_mcCommandPending = false;
            cmd = g_mcLastCmd;
            result = g_mcLastResult;
        }

        // libmc semantics: -1 means no async operation was executing; games rely
        // on it to tell idle polling apart from command completion.
        if (!hadPending)
        {
            setReturnS32(ctx, -1);
            return;
        }

        RUNTIME_LOG("[MC] Sync cmd=" << cmd << " result=" << result);

        if (cmdPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, cmdPtr))
            {
                std::memcpy(out, &cmd, sizeof(cmd));
            }
        }
        if (resultPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, resultPtr))
            {
                std::memcpy(out, &result, sizeof(result));
            }
        }

        // 1 = command finished in this runtime's immediate model.
        setReturnS32(ctx, 1);
    }

    void sceMcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                backupMcCardsLocked(); // [gta-recomp]
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = false;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdUnformat, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        const uint8_t *src = (size > 0) ? getConstMemPtr(rdram, srcAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!src)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesWritten = std::fwrite(src, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesWritten);
                if (!std::ferror(it->second.file))
                {
                    std::fflush(it->second.file);
                }
                else
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdWrite, result);
        }
        setReturnS32(ctx, 0);
    }

    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcConfigFileBytes);
    }

    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cvMcFileCursor);
    }

    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcRequiredFreeKb);
    }

    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconInfoBytes);
    }

    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconFileBytes);
    }

    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcSaveFileBytes);
    }

    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t strAddr = getRegU32(ctx, 4);
        const std::string value = readPs2CStringBounded(rdram, runtime, strAddr, 1024);
        setReturnU32(ctx, strAddr + static_cast<uint32_t>(value.size()));
    }

    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t delta = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor += delta;
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 1);
    }

    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 0);
    }

    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }
}
