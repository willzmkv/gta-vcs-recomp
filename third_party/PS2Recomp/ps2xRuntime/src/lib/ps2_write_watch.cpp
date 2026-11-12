// [gta-recomp] Guest RAM write watch - see runtime/ps2_write_watch.h. Windows: MEM_WRITE_WATCH / GetWriteWatch. Linux:
// userfaultfd's asynchronous write-protect over guest RAM (a write clears the page's protection by itself, no fault
// handler; writes by the kernel - a file read into guest RAM - work and count too) and PAGEMAP_SCAN, which lists the
// pages written and protects them again in one call (Linux 6.7+; docs/LINUX.md). Unprivileged processes get
// userfaultfd with UFFD_USER_MODE_ONLY, enough here. Where any of it is missing: not watched, as before.
#include "runtime/ps2_write_watch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "runtime/ps2_host_std.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
#define PS2X_LINUX_WRITE_WATCH 1
#endif
#endif

namespace ps2writewatch
{
    namespace
    {
        uint8_t *g_base = nullptr; // the watched allocation (nullptr: not watched)
        size_t g_bytes = 0;
        std::vector<uint32_t> g_generation;
        std::vector<void *> g_written; // GetWriteWatch's output, kept

        void publishGenerations() // [gta-recomp] the header's inline generation() / unchangedSince() read these
        {
            g_generations = g_generation.empty() ? nullptr : g_generation.data();
            g_generationPages = static_cast<uint32_t>(g_generation.size());
        }
#if defined(PS2X_LINUX_WRITE_WATCH)
        int g_uffd = -1, g_pagemap = -1;
        uint8_t *g_mapped = nullptr; // the mmap'd guest RAM (stays mapped after a failed scan ends watching)
        size_t g_mappedBytes = 0;
        std::vector<page_region> g_regions; // PAGEMAP_SCAN's output, kept

        void closeLinux()
        {
            if (g_uffd >= 0)
                close(g_uffd);
            if (g_pagemap >= 0)
                close(g_pagemap);
            g_uffd = g_pagemap = -1;
        }

        // guest RAM watched: mmap'd, registered for asynchronous write-protect and protected; null when the kernel can't
        uint8_t *allocateLinux(size_t bytes, std::string &why)
        {
            void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
            {
                why = "mmap";
                return nullptr;
            }
            auto fail = [&](const char *what) -> uint8_t * {
                why = std::string(what) + " (" + std::strerror(errno) + ")";
                closeLinux();
                munmap(p, bytes);
                return nullptr;
            };
            g_uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
            if (g_uffd < 0)
                return fail("userfaultfd");
            uffdio_api api{};
            api.api = UFFD_API;
            api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED; // untouched (zero) pages count too
            if (ioctl(g_uffd, UFFDIO_API, &api) != 0)
                return fail("UFFDIO_API (asynchronous write-protect needs Linux 6.7+)");
            uffdio_register reg{};
            reg.range = {reinterpret_cast<uint64_t>(p), bytes};
            reg.mode = UFFDIO_REGISTER_MODE_WP;
            if (ioctl(g_uffd, UFFDIO_REGISTER, &reg) != 0)
                return fail("UFFDIO_REGISTER");
            uffdio_writeprotect wp{};
            wp.range = {reinterpret_cast<uint64_t>(p), bytes};
            wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
            if (ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp) != 0)
                return fail("UFFDIO_WRITEPROTECT");
            g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
            if (g_pagemap < 0)
                return fail("/proc/self/pagemap");
            g_regions.resize(1024);
            return static_cast<uint8_t *>(p);
        }

        // the pages written since the last scan, protected again; their generations bumped. -1: the scan failed
        long scanLinux()
        {
            pm_scan_arg arg{};
            arg.size = sizeof(arg);
            arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
            arg.start = reinterpret_cast<uint64_t>(g_base);
            arg.end = arg.start + g_bytes;
            arg.vec = reinterpret_cast<uint64_t>(g_regions.data());
            arg.vec_len = g_regions.size();
            arg.category_mask = PAGE_IS_WRITTEN;
            arg.return_mask = PAGE_IS_WRITTEN;
            long pages = 0;
            while (arg.start < arg.end)
            {
                const long n = ioctl(g_pagemap, PAGEMAP_SCAN, &arg);
                if (n < 0)
                    return -1;
                for (long i = 0; i < n; ++i)
                    for (uint64_t a = g_regions[i].start; a < g_regions[i].end; a += kPageSize)
                    {
                        const size_t page = static_cast<size_t>(a - reinterpret_cast<uint64_t>(g_base)) >> kPageShift;
                        if (page < g_generation.size())
                            ++g_generation[page];
                        ++pages;
                    }
                if (arg.walk_end >= arg.end)
                    break;
                arg.start = arg.walk_end; // the output was full: go on from where the walk stopped
            }
            return pages;
        }
#endif

        struct Stats
        {
            uint64_t folds = 0, pages = 0, maxPages = 0;
            double ms = 0.0;
        } g_stats;
        auto g_reportAt = ps2x::hostNow();

        // PS2X_WRITE_WATCH_LOG=1: pages per fold and its cost, every 5 s
        void noteFold(uint64_t count, std::chrono::steady_clock::time_point start)
        {
            static const bool log = std::getenv("PS2X_WRITE_WATCH_LOG") != nullptr;
            if (!log)
                return;
            const auto now = ps2x::hostNow();
            ++g_stats.folds;
            g_stats.pages += count;
            g_stats.maxPages = std::max<uint64_t>(g_stats.maxPages, count);
            g_stats.ms += std::chrono::duration<double, std::milli>(now - start).count();
            if (now - g_reportAt > std::chrono::seconds(5))
            {
                g_reportAt = now;
                std::fprintf(stderr, "[write-watch] %llu folds: %.0f pages written per fold (max %llu of %zu), %.3f ms per fold\n",
                             (unsigned long long)g_stats.folds, g_stats.pages / double(g_stats.folds), (unsigned long long)g_stats.maxPages,
                             g_generation.size(), g_stats.ms / double(g_stats.folds));
                g_stats = Stats{};
            }
        }

        bool wanted()
        {
            const char *e = std::getenv("PS2X_WRITE_WATCH");
#if defined(_WIN32)
            return !(e && e[0] == '0');
#else
            // Linux: opt-in (PS2X_WRITE_WATCH=1). The first write to a page after each fold is a page fault there (~220 a
            // frame, on the game thread) where Windows reads dirty bits: measured, the render worker saved 0.3-0.5 ms and
            // the game thread paid 0.4-0.65 ms - no gain (docs/PERFORMANCE.md 20)
            return e && e[0] == '1';
#endif
        }
    }

    uint8_t *allocate(size_t bytes)
    {
#if defined(_WIN32)
        if (wanted() && !g_base)
        {
            void *p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
            if (p)
            {
                g_base = static_cast<uint8_t *>(p);
                g_bytes = bytes;
                g_generation.assign((bytes + kPageSize - 1u) >> kPageShift, 1u);
                publishGenerations();
                g_written.resize(g_generation.size());
                ResetWriteWatch(p, bytes); // committed memory is zero: nothing written yet
                return g_base;
            }
            std::fprintf(stderr, "[write-watch] VirtualAlloc with MEM_WRITE_WATCH failed (%lu): not watched\n", GetLastError());
        }
#elif defined(PS2X_LINUX_WRITE_WATCH)
        if (wanted() && !g_base)
        {
            std::string why;
            if (uint8_t *ram = allocateLinux(bytes, why))
            {
                g_base = g_mapped = ram;
                g_bytes = g_mappedBytes = bytes;
                g_generation.assign((bytes + kPageSize - 1u) >> kPageShift, 1u);
                publishGenerations();
                std::fprintf(stderr, "[write-watch] guest RAM watched (userfaultfd write-protect, PAGEMAP_SCAN)\n");
                return g_base; // anonymous memory: zero
            }
            std::fprintf(stderr, "[write-watch] %s failed: not watched\n", why.c_str());
        }
#endif
        uint8_t *ram = new uint8_t[bytes];
        std::memset(ram, 0, bytes);
        return ram;
    }

    void release(uint8_t *ram)
    {
        if (!ram)
            return;
#if defined(_WIN32)
        if (ram == g_base)
        {
            VirtualFree(ram, 0, MEM_RELEASE);
            g_base = nullptr;
            g_bytes = 0;
            g_generation.clear();
            publishGenerations();
            return;
        }
#elif defined(PS2X_LINUX_WRITE_WATCH)
        if (ram == g_mapped)
        {
            closeLinux();
            munmap(ram, g_mappedBytes);
            g_mapped = g_base = nullptr;
            g_mappedBytes = 0;
            g_bytes = 0;
            g_generation.clear();
            publishGenerations();
            return;
        }
#endif
        delete[] ram;
    }

    bool enabled() { return g_base != nullptr; }

    uint32_t fold()
    {
#if defined(_WIN32)
        if (!g_base)
            return 0u;
        const auto start = ps2x::hostNow();
        ULONG_PTR count = static_cast<ULONG_PTR>(g_written.size());
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base, g_bytes, g_written.data(), &count, &granularity) != 0)
            return 0u;
        for (ULONG_PTR i = 0; i < count; ++i)
        {
            const size_t page = static_cast<size_t>(static_cast<uint8_t *>(g_written[i]) - g_base) >> kPageShift;
            if (page < g_generation.size())
                ++g_generation[page];
        }
        noteFold(count, start);
        return static_cast<uint32_t>(count);
#elif defined(PS2X_LINUX_WRITE_WATCH)
        if (!g_base)
            return 0u;
        const auto start = ps2x::hostNow();
        const long count = scanLinux();
        if (count < 0)
        {
            // a failed scan could miss writes: every page counts as written from here on (nothing is skipped wrongly)
            std::fprintf(stderr, "[write-watch] PAGEMAP_SCAN failed (%s): watching stops\n", std::strerror(errno));
            for (uint32_t &g : g_generation)
                ++g;
            // [gta-recomp] and the header's inline view off: unchangedSince() false from here on, as before it was
            // inline (it tested g_base)
            g_generations = nullptr;
            g_generationPages = 0;
            closeLinux();
            g_base = nullptr; // enabled() false; the memory stays (release() frees it as plain memory below)
            return 0u;
        }
        noteFold(static_cast<uint64_t>(count), start);
        return static_cast<uint32_t>(count);
#else
        return 0u;
#endif
    }

    const uint32_t *g_generations = nullptr; // [gta-recomp] the header's inline view of g_generation (publishGenerations)
    uint32_t g_generationPages = 0;
}
