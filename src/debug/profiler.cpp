#include "profiler.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <string>
#elif defined(__linux__)
#include <csignal>
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace vcs
{
#if defined(_WIN32)
    namespace
    {
        // VCS_PROFILE_SPIKES: frame boundaries from the game thread, a slow frame's span for the sampler
        std::atomic<int64_t> g_lastFrameNs{0}, g_spikeFromNs{0}, g_spikeToNs{0};
        std::vector<std::string> g_threadNames; // of the sampled threads (sampler thread only)
        int64_t g_spikeMs = 0;
        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        // vcs.exe's function table (.pdata), looked up without RtlLookupFunctionEntry: that one takes the loader's
        // inverted-function-table lock, which the suspended game thread may hold - the runtime moves control with C++
        // exceptions, and an unwind in progress holds it - and the sampler then waited for it forever (an LTO build with
        // VCS_PROFILE_INCLUSIVE froze the game within a minute).
        struct Pdata
        {
            uintptr_t base = 0, end = 0;
            const RUNTIME_FUNCTION *table = nullptr;
            size_t count = 0;
        };
        const Pdata &mainPdata()
        {
            static const Pdata p = [] {
                Pdata d;
                const auto *image = reinterpret_cast<const unsigned char *>(GetModuleHandleW(nullptr));
                const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
                const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(image + dos->e_lfanew);
                const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                d.base = reinterpret_cast<uintptr_t>(image);
                d.end = d.base + nt->OptionalHeader.SizeOfImage;
                d.table = reinterpret_cast<const RUNTIME_FUNCTION *>(image + dir.VirtualAddress);
                d.count = dir.Size / sizeof(RUNTIME_FUNCTION);
                return d;
            }();
            return p;
        }
        PRUNTIME_FUNCTION lookupFunction(DWORD64 pc, DWORD64 *imageBase)
        {
            const Pdata &p = mainPdata();
            *imageBase = p.base;
            if (pc < p.base || pc >= p.end || !p.count)
                return nullptr;
            const DWORD rva = static_cast<DWORD>(pc - p.base);
            size_t lo = 0, hi = p.count;
            while (lo < hi)
            {
                const size_t mid = (lo + hi) / 2;
                if (rva < p.table[mid].BeginAddress)
                    hi = mid;
                else if (rva >= p.table[mid].EndAddress)
                    lo = mid + 1;
                else
                    return const_cast<PRUNTIME_FUNCTION>(&p.table[mid]);
            }
            return nullptr;
        }

        // up to n return addresses of a suspended thread's stack from its context. A fault while unwinding (a thread
        // caught where its unwind data does not describe the stack yet) ends the walk instead of the process. Only
        // through vcs.exe's code: a frame in another module (a system DLL) ends the walk.
        int unwindFrames(CONTEXT u, uintptr_t *out, int n)
        {
            int count = 0;
            __try
            {
                for (; count < n;)
                {
                    DWORD64 imageBase = 0;
                    if (u.Rip < mainPdata().base || u.Rip >= mainPdata().end)
                        break;
                    PRUNTIME_FUNCTION fe = lookupFunction(u.Rip, &imageBase);
                    if (!fe)
                    {
                        u.Rip = *reinterpret_cast<const DWORD64 *>(u.Rsp); // a leaf: the return address on top
                        u.Rsp += 8;
                    }
                    else
                    {
                        PVOID handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, u.Rip, fe, &u, &handlerData, &establisher, nullptr);
                    }
                    if (!u.Rip)
                        break;
                    out[count++] = static_cast<uintptr_t>(u.Rip);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return count;
        }

        struct HostFn
        {
            uintptr_t host;
            uint32_t guest; // lowest guest address mapped to this host function = its start
        };

        std::vector<HostFn> buildHostMap()
        {
            std::unordered_map<uintptr_t, uint32_t> lowest;
            for (uint32_t slot = 0; slot < g_ps2RecompiledFunctionTableSlotCount; ++slot)
            {
                if (auto fn = g_ps2RecompiledFunctionTable[slot])
                {
                    const uintptr_t host = reinterpret_cast<uintptr_t>(fn);
                    const uint32_t guest = g_ps2RecompiledFunctionTableBase + slot * 4u;
                    auto [it, inserted] = lowest.emplace(host, guest);
                    if (!inserted && guest < it->second)
                        it->second = guest;
                }
            }
            std::vector<HostFn> map;
            map.reserve(lowest.size());
            for (const auto &[host, guest] : lowest)
                map.push_back({host, guest});
            std::sort(map.begin(), map.end(), [](const HostFn &a, const HostFn &b) { return a.host < b.host; });
            return map;
        }

        std::vector<HANDLE> findGameThreads()
        {
            using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
            auto getDesc = reinterpret_cast<GetThreadDescriptionFn>(
                GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "GetThreadDescription"));
            std::vector<HANDLE> out;
            if (!getDesc)
                return out;
            // VCS_PROFILE_THREAD=RenderWorker: sample the render submission worker instead of the game thread; a list
            // (GameThread,RenderWorker) samples each (the spike reports say which)
            std::vector<std::wstring> wanted{L"GameThread"};
            if (const char *thread = std::getenv("VCS_PROFILE_THREAD"))
            {
                wanted.clear();
                for (const char *p = thread; *p;)
                {
                    const char *comma = std::strchr(p, ',');
                    const char *e = comma ? comma : p + std::strlen(p);
                    wanted.emplace_back(p, e);
                    p = comma ? comma + 1 : e;
                }
            }
            g_threadNames.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{sizeof(te)};
            const DWORD pid = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid)
                    continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h)
                    continue;
                PWSTR name = nullptr;
                bool keep = false;
                std::string label;
                if (SUCCEEDED(getDesc(h, &name)) && name)
                {
                    for (const std::wstring &w : wanted)
                        keep |= w == name;
                    for (const wchar_t *c = name; *c; ++c)
                        label += static_cast<char>(*c);
                    LocalFree(name);
                }
                if (keep)
                {
                    out.push_back(h);
                    g_threadNames.push_back(label);
                }
                else
                    CloseHandle(h);
            }
            CloseHandle(snap);
            return out;
        }
    }

    void startProfiler(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_PROFILE");
        if (!env || !*env)
            return;
        const double reportSeconds = std::atof(env) > 0.0 ? std::atof(env) : 5.0;
        if (const char *spikes = std::getenv("VCS_PROFILE_SPIKES"))
            g_spikeMs = std::atoll(spikes);

        std::thread([&runtime, reportSeconds]()
                    {
            const std::vector<HostFn> map = buildHostMap();
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            std::vector<HANDLE> threads;
            // is the host function at `host` generated code (FUN_*), not a hook installed with replaceFunction - and how
            // long is it? [gta-recomp] A sample counts for a generated function only within the function's own symbol
            // size: in a ThinLTO / PGO build the linker places hot runtime functions between generated ones, and
            // "the nearest generated function below, within 1 MB" counted them as guest code.
            struct GeneratedInfo
            {
                bool generated = false;
                uint64_t size = 0x100000u; // unknown size: the old 1 MB window
            };
            std::unordered_map<uintptr_t, GeneratedInfo> generatedCache;
            auto generatedInfo = [&generatedCache](uintptr_t host) -> const GeneratedInfo & {
                auto found = generatedCache.find(host);
                if (found != generatedCache.end())
                    return found->second;
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 displacement = 0;
                GeneratedInfo info;
                const bool known = SymFromAddr(GetCurrentProcess(), host, &displacement, sym) != 0;
                info.generated = !known || std::strncmp(sym->Name, "FUN_", 4) == 0 || std::strstr(sym->Name, "::FUN_") != nullptr;
                if (known && sym->Size && displacement == 0)
                    info.size = sym->Size;
                return generatedCache.emplace(host, info).first->second;
            };
            // pc inside the generated function that starts at `host`
            auto withinGenerated = [&generatedInfo](uintptr_t host, uintptr_t pc) {
                const GeneratedInfo &info = generatedInfo(host);
                return info.generated && pc - host < info.size;
            };
            // VCS_PROFILE_CALLERS=name[,name...]: for samples in host functions whose name contains one of these, the call
            // stack (unwound up to the first generated function) - who calls memchr / memmove / the allocator
            std::vector<std::string> callersOf;
            if (const char *e = std::getenv("VCS_PROFILE_CALLERS"))
                for (const char *p = e; *p;)
                {
                    const char *comma = std::strchr(p, ',');
                    callersOf.emplace_back(p, comma ? comma : p + std::strlen(p));
                    p = comma ? comma + 1 : p + std::strlen(p);
                }
            std::unordered_map<uintptr_t, bool> callerMatch; // host RIP -> its function is one of callersOf
            auto symbolName = [](uintptr_t addr) -> std::string {
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 displacement = 0;
                return SymFromAddr(GetCurrentProcess(), addr, &displacement, sym) ? std::string(sym->Name) : std::string("?");
            };
            std::map<std::vector<uintptr_t>, uint64_t> stacks; // frames (innermost first) -> samples
            std::unordered_map<uint32_t, uint64_t> hits;    // guest function -> samples
            // VCS_PROFILE_INCLUSIVE=1: every guest function on the sampled stack counts once per sample (generated
            // functions call each other as host functions, so the host stack holds the guest call chain) - the cost of
            // a subsystem with all it calls
            static const bool inclusive = std::getenv("VCS_PROFILE_INCLUSIVE") != nullptr;
            std::unordered_map<uint32_t, uint64_t> inclusiveHits;
            std::unordered_map<uintptr_t, uint64_t> other;  // non-generated host RIP (page) -> samples
            uint64_t total = 0;
            auto lastReport = std::chrono::steady_clock::now();
            struct Timed // VCS_PROFILE_SPIKES: recent samples with their time
            {
                int64_t ns;
                uint32_t guest; // 0: host code
                uintptr_t rip;
                uint32_t thread; // index in threads
                uint32_t chain;  // chainNames[chain - 1] (0: none)
            };
            std::vector<Timed> recent;
            std::map<std::vector<uintptr_t>, uint32_t> chainIds;
            std::vector<std::string> chainNames;
            int spikesReported = 0;
            while (!runtime.isStopRequested())
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                // until every thread asked for exists (the render worker starts after the game thread): look again each second
                static const size_t wantedCount = [] {
                    const char *t = std::getenv("VCS_PROFILE_THREAD");
                    size_t n = 1;
                    for (const char *p = t; p && *p; ++p)
                        n += *p == ',';
                    return n;
                }();
                static auto lastLook = std::chrono::steady_clock::time_point{};
                if (threads.size() < wantedCount && std::chrono::steady_clock::now() - lastLook > std::chrono::seconds(1))
                {
                    lastLook = std::chrono::steady_clock::now();
                    for (HANDLE h : threads)
                        CloseHandle(h);
                    threads = findGameThreads();
                }
                if (threads.empty())
                    continue;
                for (size_t ti = 0; ti < threads.size(); ++ti)
                {
                    HANDLE h = threads[ti];
                    if (SuspendThread(h) == static_cast<DWORD>(-1))
                        continue;
                    uint32_t sampleChain = 0; // (VCS_PROFILE_SPIKES with VCS_PROFILE_CALLERS: its call chain, 1-based)
                    CONTEXT c{};
                    c.ContextFlags = callersOf.empty() && !inclusive ? CONTEXT_CONTROL : CONTEXT_FULL;
                    const bool got = GetThreadContext(h, &c) != 0;
                    const uintptr_t rip = c.Rip;
                    auto it = std::upper_bound(map.begin(), map.end(), rip,
                                               [](uintptr_t v, const HostFn &f) { return v < f.host; });
                    auto inGenerated = [&](uintptr_t pc) {
                        auto f = std::upper_bound(map.begin(), map.end(), pc, [](uintptr_t v, const HostFn &fn) { return v < fn.host; });
                        return f != map.begin() && f != map.end() && withinGenerated(std::prev(f)->host, pc);
                    };
                    if (got && !callersOf.empty() && !inGenerated(rip))
                    {
                        auto m = callerMatch.find(rip);
                        if (m == callerMatch.end())
                        {
                            const std::string name = symbolName(rip);
                            bool match = false;
                            for (const std::string &want : callersOf)
                                match |= name.find(want) != std::string::npos;
                            m = callerMatch.emplace(rip, match).first;
                        }
                        if (m->second) // unwind while the thread is still suspended
                        {
                            uintptr_t raw[12];
                            const int n = unwindFrames(c, raw, 12);
                            std::vector<uintptr_t> frames{rip}; // the sampled function first, then its callers
                            for (int i = 0; i < n; ++i)
                            {
                                frames.push_back(raw[i]);
                                if (inGenerated(raw[i]))
                                    break;
                            }
                            ++stacks[frames];
                            if (g_spikeMs > 0) // a spike's report names the chain (VCS_PROFILE_CALLERS)
                            {
                                auto found = chainIds.find(frames);
                                if (found == chainIds.end())
                                {
                                    std::string chain;
                                    for (size_t k = 1; k < frames.size() && k < 9; ++k)
                                    {
                                        auto f = std::upper_bound(map.begin(), map.end(), frames[k],
                                                                  [](uintptr_t v, const HostFn &fn) { return v < fn.host; });
                                        const bool gen = f != map.begin() && f != map.end() && withinGenerated(std::prev(f)->host, frames[k]);
                                        char guest[24];
                                        if (gen)
                                            std::snprintf(guest, sizeof(guest), "guest 0x%08x", std::prev(f)->guest);
                                        chain += " < " + (gen ? std::string(guest) : symbolName(frames[k]));
                                    }
                                    chainNames.push_back(chain);
                                    found = chainIds.emplace(frames, static_cast<uint32_t>(chainNames.size())).first;
                                }
                                sampleChain = found->second;
                            }
                        }
                    }
                    if (got && inclusive)
                    {
                        uintptr_t raw[64];
                        const int n = unwindFrames(c, raw, 64);
                        uint32_t seen[65];
                        int nseen = 0;
                        for (int i = -1; i < n; ++i)
                        {
                            const uintptr_t pc = i < 0 ? rip : raw[i];
                            auto f = std::upper_bound(map.begin(), map.end(), pc, [](uintptr_t v, const HostFn &fn) { return v < fn.host; });
                            if (f == map.begin() || f == map.end() || !withinGenerated(std::prev(f)->host, pc))
                                continue;
                            const uint32_t guest = std::prev(f)->guest;
                            if (std::find(seen, seen + nseen, guest) == seen + nseen)
                            {
                                seen[nseen++] = guest;
                                ++inclusiveHits[guest];
                            }
                        }
                    }
                    ResumeThread(h);
                    if (!got)
                        continue;
                    ++total;
                    // Generated functions are at most a few hundred KB of host code each.
                    // (not past the last one: whatever the linker placed after it - e.g. compiled VU1 code - was
                    // counted as that function before)
                    // (a replaced function's slot points at a host hook outside the generated code: a sample after it
                    // is host code, not that guest function - checked by the symbol name below)
                    const bool generated = it != map.begin() && it != map.end() && withinGenerated(std::prev(it)->host, rip);
                    if (generated)
                        ++hits[std::prev(it)->guest];
                    else
                        ++other[rip];
                    if (g_spikeMs > 0)
                    {
                        if (recent.size() >= 8192u)
                            recent.erase(recent.begin(), recent.begin() + 4096);
                        recent.push_back({nowNs(), generated ? std::prev(it)->guest : 0u, rip, static_cast<uint32_t>(ti), sampleChain});
                    }
                }
                if (g_spikeMs > 0 && g_spikeToNs.load(std::memory_order_acquire) != 0)
                {
                    const int64_t from = g_spikeFromNs.load(), to = g_spikeToNs.load();
                    g_spikeToNs.store(0, std::memory_order_release);
                    std::unordered_map<std::string, uint64_t> by;
                    uint64_t n = 0;
                    for (const Timed &t : recent)
                        if (t.ns >= from && t.ns <= to)
                        {
                            ++n;
                            char key[32];
                            if (t.guest)
                                std::snprintf(key, sizeof(key), "guest 0x%08x", t.guest);
                            const std::string who = threads.size() > 1 && t.thread < g_threadNames.size() ? g_threadNames[t.thread] + ": " : "";
                            by[who + (t.guest ? std::string(key) : "host " + symbolName(t.rip)) +
                               (t.chain && t.chain <= chainNames.size() ? chainNames[t.chain - 1] : std::string())]++;
                        }
                    if (n && spikesReported++ < 400)
                    {
                        std::vector<std::pair<std::string, uint64_t>> rows(by.begin(), by.end());
                        std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second > b.second; });
                        std::string line;
                        for (size_t i = 0; i < rows.size() && i < 12; ++i)
                            line += " | " + rows[i].first + " " + std::to_string(rows[i].second);
                        static const int64_t startNs = nowNs();
                        std::fprintf(stderr, "[spike] t=%.2f s: %.1f ms frame, %llu samples%s\n", (to - startNs) / 1e9, (to - from) / 1e6,
                                     static_cast<unsigned long long>(n), line.c_str());
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - lastReport).count() < reportSeconds || total == 0)
                    continue;
                lastReport = now;
                std::vector<std::pair<uint32_t, uint64_t>> top(hits.begin(), hits.end());
                std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second > b.second; });
                uint64_t guestTotal = 0;
                for (const auto &[pc, n] : hits)
                    guestTotal += n;
                std::fprintf(stderr, "[profile] %llu samples, %.1f%% in generated (guest) code\n", static_cast<unsigned long long>(total),
                             100.0 * guestTotal / total);
                for (size_t i = 0; i < top.size() && i < 15; ++i)
                    std::fprintf(stderr, "[profile]   %5.1f%%  guest 0x%08x\n", 100.0 * top[i].second / total, top[i].first);
                if (inclusive && total)
                {
                    std::vector<std::pair<uint32_t, uint64_t>> inc(inclusiveHits.begin(), inclusiveHits.end());
                    std::sort(inc.begin(), inc.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    for (size_t i = 0; i < inc.size() && i < 60; ++i)
                        std::fprintf(stderr, "[profile]   %5.1f%%  inclusive guest 0x%08x\n", 100.0 * inc[i].second / total, inc[i].first);
                    inclusiveHits.clear();
                }
                // VCS_PROFILE_GUEST_FILE=<path>: every guest function's samples, appended per report window ("window <s> total
                // N" then "0xADDR N" lines; s = seconds since the profiler started) - for summing by subsystem offline
                static const char *guestFile = std::getenv("VCS_PROFILE_GUEST_FILE");
                static const auto profileStart = std::chrono::steady_clock::now();
                if (guestFile && *guestFile)
                    if (FILE *f = std::fopen(guestFile, "a"))
                    {
                        std::fprintf(f, "window %.0f total %llu\n", std::chrono::duration<double>(now - profileStart).count(),
                                     static_cast<unsigned long long>(total));
                        for (const auto &[pc, n] : top)
                            std::fprintf(f, "0x%08x %llu\n", pc, static_cast<unsigned long long>(n));
                        std::fclose(f);
                    }
                // Resolve non-generated samples to host symbols (needs the PDB; see VCS_RUNTIME_DEBUG_INFO).
                std::unordered_map<std::string, uint64_t> byName;
                for (const auto &[rip, count] : other)
                {
                    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 255;
                    DWORD64 displacement = 0;
                    std::string name;
                    if (SymFromAddr(GetCurrentProcess(), rip, &displacement, sym))
                        name = sym->Name;
                    else
                    {
                        HMODULE mod = nullptr;
                        char modName[MAX_PATH] = "?";
                        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCSTR>(rip), &mod))
                            GetModuleFileNameA(mod, modName, MAX_PATH);
                        const char *leaf = std::strrchr(modName, '\\');
                        name = std::string(leaf ? leaf + 1 : modName) + "!?";
                    }
                    byName[name] += count;
                }
                std::vector<std::pair<std::string, uint64_t>> rest(byName.begin(), byName.end());
                std::sort(rest.begin(), rest.end(), [](auto &a, auto &b) { return a.second > b.second; });
                for (size_t i = 0; i < rest.size() && i < 30; ++i)
                    std::fprintf(stderr, "[profile]   %5.1f%%  host %s\n", 100.0 * rest[i].second / total, rest[i].first.c_str());
                // VCS_PROFILE_LINES=<name>: the source lines of the host functions whose name contains it
                static const char *linesOf = std::getenv("VCS_PROFILE_LINES");
                if (linesOf && *linesOf)
                {
                    std::unordered_map<std::string, uint64_t> byLine;
                    for (const auto &[rip, count] : other)
                    {
                        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                        sym->MaxNameLen = 255;
                        DWORD64 displacement = 0;
                        if (!SymFromAddr(GetCurrentProcess(), rip, &displacement, sym) || !std::strstr(sym->Name, linesOf))
                            continue;
                        IMAGEHLP_LINE64 line{};
                        line.SizeOfStruct = sizeof(line);
                        DWORD lineDisplacement = 0;
                        std::string key = "?";
                        if (SymGetLineFromAddr64(GetCurrentProcess(), rip, &lineDisplacement, &line))
                        {
                            const char *leaf = std::strrchr(line.FileName, '\\');
                            key = std::string(leaf ? leaf + 1 : line.FileName) + ":" + std::to_string(line.LineNumber);
                        }
                        byLine[key] += count;
                    }
                    std::vector<std::pair<std::string, uint64_t>> lines(byLine.begin(), byLine.end());
                    std::sort(lines.begin(), lines.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    for (size_t i = 0; i < lines.size() && i < 25; ++i)
                        std::fprintf(stderr, "[profile]     %5.1f%%  line %s\n", 100.0 * lines[i].second / total, lines[i].first.c_str());
                }
                if (!stacks.empty())
                {
                    // by caller chain, names resolved now (a guest function by its address)
                    std::unordered_map<std::string, uint64_t> byChain;
                    for (const auto &[frames, count] : stacks)
                    {
                        std::string chain;
                        for (uintptr_t pc : frames)
                        {
                            auto f = std::upper_bound(map.begin(), map.end(), pc, [](uintptr_t v, const HostFn &fn) { return v < fn.host; });
                            const bool gen = f != map.begin() && f != map.end() && withinGenerated(std::prev(f)->host, pc);
                            char guest[24];
                            if (gen)
                                std::snprintf(guest, sizeof(guest), "guest 0x%08x", std::prev(f)->guest);
                            chain += (chain.empty() ? "" : " < ") + (gen ? std::string(guest) : symbolName(pc));
                        }
                        byChain[chain] += count;
                    }
                    std::vector<std::pair<std::string, uint64_t>> chains(byChain.begin(), byChain.end());
                    std::sort(chains.begin(), chains.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    for (size_t i = 0; i < chains.size() && i < 80; ++i)
                        std::fprintf(stderr, "[profile]     %5.1f%%  called from %s\n", 100.0 * chains[i].second / total, chains[i].first.c_str());
                    stacks.clear();
                }
                std::fflush(stderr);
                hits.clear();
                other.clear();
                total = 0;
            } })
            .detach();
    }
    void profilerFrame()
    {
        if (g_spikeMs <= 0)
            return;
        const int64_t now = nowNs(), last = g_lastFrameNs.exchange(now);
        if (last && now - last > g_spikeMs * 1000000 && g_spikeToNs.load(std::memory_order_acquire) == 0)
        {
            g_spikeFromNs.store(last);
            g_spikeToNs.store(now, std::memory_order_release);
        }
    }
#elif defined(__linux__)
    // Linux: the sampler thread sends SIGPROF to each sampled thread (tgkill); the handler hands its RIP back. Same reports
    // as on Windows (guest functions, host symbols, [spike] frames, VCS_PROFILE_GUEST_FILE); VCS_PROFILE_CALLERS and
    // VCS_PROFILE_LINES are Windows only - `perf record -g` covers them here (docs/LINUX.md).
    namespace
    {
        std::atomic<int64_t> g_lastFrameNs{0}, g_spikeFromNs{0}, g_spikeToNs{0};
        int64_t g_spikeMs = 0;
        std::atomic<uintptr_t> g_sampledRip{0};
        std::atomic<bool> g_sampleReady{false};

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void onSample(int, siginfo_t *, void *context)
        {
            const ucontext_t *uc = static_cast<const ucontext_t *>(context);
            g_sampledRip.store(static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]), std::memory_order_relaxed);
            g_sampleReady.store(true, std::memory_order_release);
        }

        struct HostFn
        {
            uintptr_t host;
            uint32_t guest; // lowest guest address mapped to this host function = its start
        };

        std::vector<HostFn> buildHostMap()
        {
            std::unordered_map<uintptr_t, uint32_t> lowest;
            for (uint32_t slot = 0; slot < g_ps2RecompiledFunctionTableSlotCount; ++slot)
                if (auto fn = g_ps2RecompiledFunctionTable[slot])
                {
                    const uintptr_t host = reinterpret_cast<uintptr_t>(fn);
                    const uint32_t guest = g_ps2RecompiledFunctionTableBase + slot * 4u;
                    auto [it, inserted] = lowest.emplace(host, guest);
                    if (!inserted && guest < it->second)
                        it->second = guest;
                }
            std::vector<HostFn> map;
            for (const auto &[host, guest] : lowest)
                map.push_back({host, guest});
            std::sort(map.begin(), map.end(), [](const HostFn &a, const HostFn &b) { return a.host < b.host; });
            return map;
        }

        // the threads named in VCS_PROFILE_THREAD (default GameThread), by /proc/self/task/*/comm
        std::vector<std::pair<pid_t, std::string>> findThreads()
        {
            std::vector<std::string> wanted{"GameThread"};
            if (const char *thread = std::getenv("VCS_PROFILE_THREAD"))
            {
                wanted.clear();
                for (const char *p = thread; *p;)
                {
                    const char *comma = std::strchr(p, ',');
                    const char *e = comma ? comma : p + std::strlen(p);
                    wanted.emplace_back(p, e);
                    p = comma ? comma + 1 : e;
                }
            }
            std::vector<std::pair<pid_t, std::string>> out;
            DIR *dir = opendir("/proc/self/task");
            if (!dir)
                return out;
            while (dirent *e = readdir(dir))
            {
                if (e->d_name[0] == '.')
                    continue;
                char path[64], name[32] = {};
                std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
                if (FILE *f = std::fopen(path, "r"))
                {
                    if (std::fgets(name, sizeof(name), f))
                        name[std::strcspn(name, "\n")] = 0;
                    std::fclose(f);
                }
                for (const std::string &w : wanted)
                    if (w == name)
                        out.emplace_back(static_cast<pid_t>(std::atoi(e->d_name)), name);
            }
            closedir(dir);
            return out;
        }

        // A host address's symbol (demangled) and its function's size, from the exe's dynamic symbols (-rdynamic)
        bool hostSymbol(uintptr_t addr, std::string *name, uintptr_t *start, uint64_t *size)
        {
            Dl_info info{};
            void *entry = nullptr;
            if (!dladdr1(reinterpret_cast<void *>(addr), &info, &entry, RTLD_DL_SYMENT) || !info.dli_sname)
                return false;
            const ElfW(Sym) *sym = static_cast<const ElfW(Sym) *>(entry);
            if (name)
            {
                int status = 0;
                char *plain = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                *name = status == 0 && plain ? plain : info.dli_sname;
                std::free(plain);
            }
            if (start)
                *start = reinterpret_cast<uintptr_t>(info.dli_saddr);
            if (size)
                *size = sym ? sym->st_size : 0;
            return true;
        }
    }

    void startProfiler(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_PROFILE");
        if (!env || !*env)
            return;
        const double reportSeconds = std::atof(env) > 0.0 ? std::atof(env) : 5.0;
        if (const char *spikes = std::getenv("VCS_PROFILE_SPIKES"))
            g_spikeMs = std::atoll(spikes);
        if (std::getenv("VCS_PROFILE_CALLERS") || std::getenv("VCS_PROFILE_LINES"))
            std::fprintf(stderr, "[profile] VCS_PROFILE_CALLERS / VCS_PROFILE_LINES are Windows only: use perf record -g on Linux\n");
        struct sigaction sa{};
        sa.sa_sigaction = &onSample;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);

        std::thread([&runtime, reportSeconds]() {
            const std::vector<HostFn> map = buildHostMap();
            const pid_t pid = getpid();
            // is the host function at `host` generated code (FUN_*, not a hook), and how long is it (symbol size)
            struct GeneratedInfo
            {
                bool generated = false;
                uint64_t size = 0x100000u; // unknown size: a 1 MB window
            };
            std::unordered_map<uintptr_t, GeneratedInfo> generatedCache;
            auto withinGenerated = [&generatedCache](uintptr_t host, uintptr_t pc) {
                auto found = generatedCache.find(host);
                if (found == generatedCache.end())
                {
                    GeneratedInfo info;
                    std::string name;
                    uintptr_t start = 0;
                    uint64_t size = 0;
                    const bool known = hostSymbol(host, &name, &start, &size);
                    // generated functions are exported (-rdynamic): one dladdr cannot name is a hook (an anonymous
                    // namespace's replaceFunction target) - counting it as generated with a 1 MB window charged the host
                    // code after it to the hooked guest function (FUN_003298D8's 5%, PERFORMANCE.md 28)
                    info.generated = known && name.find("FUN_") != std::string::npos;
                    if (known && size && start == host)
                        info.size = size;
                    found = generatedCache.emplace(host, info).first;
                }
                return found->second.generated && pc - host < found->second.size;
            };
            auto symbolName = [](uintptr_t addr) {
                std::string name;
                if (hostSymbol(addr, &name, nullptr, nullptr))
                    return name;
                Dl_info info{};
                if (dladdr(reinterpret_cast<void *>(addr), &info) && info.dli_fname)
                {
                    const char *leaf = std::strrchr(info.dli_fname, '/');
                    return std::string(leaf ? leaf + 1 : info.dli_fname) + "!?";
                }
                return std::string("?");
            };
            std::vector<std::pair<pid_t, std::string>> threads;
            std::unordered_map<uint32_t, uint64_t> hits;   // guest function -> samples
            std::unordered_map<uintptr_t, uint64_t> other; // host RIP -> samples
            uint64_t total = 0;
            auto lastReport = std::chrono::steady_clock::now(), lastLook = std::chrono::steady_clock::time_point{};
            struct Timed
            {
                int64_t ns;
                uint32_t guest; // 0: host code
                uintptr_t rip;
                uint32_t thread;
            };
            std::vector<Timed> recent;
            int spikesReported = 0;
            size_t wantedCount = 1;
            for (const char *p = std::getenv("VCS_PROFILE_THREAD"); p && *p; ++p)
                wantedCount += *p == ',';
            while (!runtime.isStopRequested())
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                if (threads.size() < wantedCount && std::chrono::steady_clock::now() - lastLook > std::chrono::seconds(1))
                {
                    lastLook = std::chrono::steady_clock::now();
                    threads = findThreads();
                }
                for (size_t ti = 0; ti < threads.size(); ++ti)
                {
                    g_sampleReady.store(false, std::memory_order_relaxed);
                    if (syscall(SYS_tgkill, pid, threads[ti].first, SIGPROF) != 0)
                    {
                        threads.clear(); // gone: looked for again
                        break;
                    }
                    const int64_t deadline = nowNs() + 2000000;
                    while (!g_sampleReady.load(std::memory_order_acquire) && nowNs() < deadline)
                        std::this_thread::yield();
                    if (!g_sampleReady.load(std::memory_order_acquire))
                        continue;
                    const uintptr_t rip = g_sampledRip.load(std::memory_order_relaxed);
                    ++total;
                    auto it = std::upper_bound(map.begin(), map.end(), rip, [](uintptr_t v, const HostFn &f) { return v < f.host; });
                    const bool generated = it != map.begin() && it != map.end() && withinGenerated(std::prev(it)->host, rip);
                    if (generated)
                        ++hits[std::prev(it)->guest];
                    else
                        ++other[rip];
                    if (g_spikeMs > 0)
                    {
                        if (recent.size() >= 8192u)
                            recent.erase(recent.begin(), recent.begin() + 4096);
                        recent.push_back({nowNs(), generated ? std::prev(it)->guest : 0u, rip, static_cast<uint32_t>(ti)});
                    }
                }
                if (g_spikeMs > 0 && g_spikeToNs.load(std::memory_order_acquire) != 0)
                {
                    const int64_t from = g_spikeFromNs.load(), to = g_spikeToNs.load();
                    g_spikeToNs.store(0, std::memory_order_release);
                    std::unordered_map<std::string, uint64_t> by;
                    uint64_t n = 0;
                    for (const Timed &t : recent)
                        if (t.ns >= from && t.ns <= to)
                        {
                            ++n;
                            char key[32];
                            if (t.guest)
                                std::snprintf(key, sizeof(key), "guest 0x%08x", t.guest);
                            const std::string who = threads.size() > 1 && t.thread < threads.size() ? threads[t.thread].second + ": " : "";
                            by[who + (t.guest ? std::string(key) : "host " + symbolName(t.rip))]++;
                        }
                    if (n && spikesReported++ < 400)
                    {
                        std::vector<std::pair<std::string, uint64_t>> rows(by.begin(), by.end());
                        std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second > b.second; });
                        std::string line;
                        for (size_t i = 0; i < rows.size() && i < 12; ++i)
                            line += " | " + rows[i].first + " " + std::to_string(rows[i].second);
                        static const int64_t startNs = nowNs();
                        std::fprintf(stderr, "[spike] t=%.2f s: %.1f ms frame, %llu samples%s\n", (to - startNs) / 1e9, (to - from) / 1e6,
                                     static_cast<unsigned long long>(n), line.c_str());
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - lastReport).count() < reportSeconds || total == 0)
                    continue;
                lastReport = now;
                std::vector<std::pair<uint32_t, uint64_t>> top(hits.begin(), hits.end());
                std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second > b.second; });
                uint64_t guestTotal = 0;
                for (const auto &[pc, n] : hits)
                    guestTotal += n;
                std::fprintf(stderr, "[profile] %llu samples, %.1f%% in generated (guest) code\n", static_cast<unsigned long long>(total),
                             100.0 * guestTotal / total);
                for (size_t i = 0; i < top.size() && i < 15; ++i)
                    std::fprintf(stderr, "[profile]   %5.1f%%  guest 0x%08x\n", 100.0 * top[i].second / total, top[i].first);
                static const char *guestFile = std::getenv("VCS_PROFILE_GUEST_FILE");
                static const auto profileStart = std::chrono::steady_clock::now();
                if (guestFile && *guestFile)
                    if (FILE *f = std::fopen(guestFile, "a"))
                    {
                        std::fprintf(f, "window %.0f total %llu\n", std::chrono::duration<double>(now - profileStart).count(),
                                     static_cast<unsigned long long>(total));
                        for (const auto &[pc, n] : top)
                            std::fprintf(f, "0x%08x %llu\n", pc, static_cast<unsigned long long>(n));
                        std::fclose(f);
                    }
                std::unordered_map<std::string, uint64_t> byName;
                for (const auto &[rip, count] : other)
                    byName[symbolName(rip)] += count;
                std::vector<std::pair<std::string, uint64_t>> rest(byName.begin(), byName.end());
                std::sort(rest.begin(), rest.end(), [](auto &a, auto &b) { return a.second > b.second; });
                for (size_t i = 0; i < rest.size() && i < 30; ++i)
                    std::fprintf(stderr, "[profile]   %5.1f%%  host %s\n", 100.0 * rest[i].second / total, rest[i].first.c_str());
                // VCS_PROFILE_RAW=<file>: the samples dladdr could not name, as "module offset count" lines appended per
                // report - for `llvm-symbolizer --obj=<module>` (functions in anonymous namespaces, static libraries)
                static const char *rawPath = std::getenv("VCS_PROFILE_RAW");
                if (rawPath && *rawPath)
                    if (FILE *f = std::fopen(rawPath, "a"))
                    {
                        for (const auto &[rip, count] : other)
                        {
                            std::string name;
                            if (hostSymbol(rip, &name, nullptr, nullptr))
                                continue;
                            Dl_info info{};
                            if (dladdr(reinterpret_cast<void *>(rip), &info) && info.dli_fname && info.dli_fbase)
                                std::fprintf(f, "%s 0x%llx %llu\n", info.dli_fname,
                                             static_cast<unsigned long long>(rip - reinterpret_cast<uintptr_t>(info.dli_fbase)),
                                             static_cast<unsigned long long>(count));
                        }
                        std::fclose(f);
                    }
                std::fflush(stderr);
                hits.clear();
                other.clear();
                total = 0;
            }
        }).detach();
    }

    void profilerFrame()
    {
        if (g_spikeMs <= 0)
            return;
        const int64_t now = nowNs(), last = g_lastFrameNs.exchange(now);
        if (last && now - last > g_spikeMs * 1000000 && g_spikeToNs.load(std::memory_order_acquire) == 0)
        {
            g_spikeFromNs.store(last);
            g_spikeToNs.store(now, std::memory_order_release);
        }
    }
#else
    void startProfiler(PS2Runtime &) {}
    void profilerFrame() {}
#endif
}
