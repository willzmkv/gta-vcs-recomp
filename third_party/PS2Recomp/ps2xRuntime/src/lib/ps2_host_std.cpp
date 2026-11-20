// [gta-recomp] See runtime/ps2_host_std.h. Windows with libc++ only (llvm-mingw); elsewhere the header is inline.
#include "runtime/ps2_host_std.h"

#include <cstdint>

#if PS2X_HOST_TIME_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ps2x
{
    namespace
    {
        int64_t qpcFrequency() noexcept
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }
    }

    std::chrono::steady_clock::time_point hostNow() noexcept
    {
        // fixed at boot (Windows' documentation); a local static, as other files' globals read the clock while they
        // are initialised
        static const int64_t f = qpcFrequency();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        // libc++'s own formula (whole seconds, then the fraction), so the values equal steady_clock::now's; 10 MHz,
        // what Windows 10 and later report, is the same as a multiply
        const int64_t ns = f == 10000000 ? c.QuadPart * 100
                                         : c.QuadPart / f * 1000000000 + c.QuadPart % f * 1000000000 / f;
        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
    }

    bool waitMs(std::condition_variable &cv, std::unique_lock<std::mutex> &lock, unsigned long ms) noexcept
    {
        // libc++'s condition_variable and mutex are a CONDITION_VARIABLE and an SRWLOCK on Windows (what its own
        // __libcpp_condvar_timedwait passes); the lock stays held across the call as the wait expects
        return SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(cv.native_handle()),
                                         reinterpret_cast<PSRWLOCK>(lock.mutex()->native_handle()), ms, 0) != 0;
    }
}
#endif
