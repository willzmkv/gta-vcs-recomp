// [gta-recomp] Host clock, timed waits and a recursive lock that cost the same with every Windows C++ library
// (docs/PERFORMANCE.md 29). llvm-mingw's libc++ makes steady_clock::now an out-of-line call with two 64-bit divisions
// (MSVC's STL: inline, a multiply for the usual 10 MHz QueryPerformanceCounter); its timed condition-variable waits
// truncate the timeout to whole milliseconds through system_clock, so a wait with less than 1 ms left returns at once
// and a wait_for(1 ms) loop or the last millisecond of a wait_until spins; its recursive_mutex is a CRITICAL_SECTION
// (MSVC's: an SRW lock). hostNow() / waitUntil() / waitFor() / RecursiveMutex are steady_clock::now,
// condition_variable's waits and recursive_mutex everywhere else; on Windows with libc++ they go to
// QueryPerformanceCounter, SleepConditionVariableSRW with the timeout rounded up (as MSVC's STL does) and a std::mutex
// (an SRW lock) with an owner and a depth.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>

#if defined(_WIN32) && defined(_LIBCPP_VERSION)
#define PS2X_HOST_TIME_WIN32 1
#endif

namespace ps2x
{
#if PS2X_HOST_TIME_WIN32
    std::chrono::steady_clock::time_point hostNow() noexcept; // the same values as steady_clock::now
    // false: timed out. Waits at most `ms` (the caller rounds up); wakes may be spurious.
    bool waitMs(std::condition_variable &cv, std::unique_lock<std::mutex> &lock, unsigned long ms) noexcept;
#else
    inline std::chrono::steady_clock::time_point hostNow() noexcept { return std::chrono::steady_clock::now(); }
#endif

    template <class Clock, class Duration>
    std::cv_status waitUntil(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                             const std::chrono::time_point<Clock, Duration> &deadline)
    {
#if PS2X_HOST_TIME_WIN32
        static_assert(std::is_same_v<Clock, std::chrono::steady_clock>, "steady_clock deadlines only");
        const auto now = hostNow();
        if (now >= deadline)
            return std::cv_status::timeout;
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        waitMs(cv, lock, left >= 0x7fffffff ? 0x7fffffffUL : static_cast<unsigned long>(left));
        return hostNow() >= deadline ? std::cv_status::timeout : std::cv_status::no_timeout;
#else
        return cv.wait_until(lock, deadline);
#endif
    }

    template <class Clock, class Duration, class Predicate>
    bool waitUntil(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                   const std::chrono::time_point<Clock, Duration> &deadline, Predicate pred)
    {
#if PS2X_HOST_TIME_WIN32
        while (!pred())
            if (waitUntil(cv, lock, deadline) == std::cv_status::timeout)
                return pred();
        return true;
#else
        return cv.wait_until(lock, deadline, std::move(pred));
#endif
    }

    template <class Rep, class Period>
    std::cv_status waitFor(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                           const std::chrono::duration<Rep, Period> &d)
    {
#if PS2X_HOST_TIME_WIN32
        return waitUntil(cv, lock, hostNow() + std::chrono::ceil<std::chrono::steady_clock::duration>(d));
#else
        return cv.wait_for(lock, d);
#endif
    }

    template <class Rep, class Period, class Predicate>
    bool waitFor(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                 const std::chrono::duration<Rep, Period> &d, Predicate pred)
    {
#if PS2X_HOST_TIME_WIN32
        return waitUntil(cv, lock, hostNow() + std::chrono::ceil<std::chrono::steady_clock::duration>(d), std::move(pred));
#else
        return cv.wait_for(lock, d, std::move(pred));
#endif
    }

#if PS2X_HOST_TIME_WIN32
    class RecursiveMutex
    {
    public:
        void lock()
        {
            const std::thread::id me = std::this_thread::get_id();
            if (m_owner.load(std::memory_order_relaxed) == me) // only this thread ever stores its own id
            {
                ++m_depth;
                return;
            }
            m_mutex.lock();
            m_owner.store(me, std::memory_order_relaxed);
            m_depth = 1;
        }
        bool try_lock()
        {
            const std::thread::id me = std::this_thread::get_id();
            if (m_owner.load(std::memory_order_relaxed) == me)
            {
                ++m_depth;
                return true;
            }
            if (!m_mutex.try_lock())
                return false;
            m_owner.store(me, std::memory_order_relaxed);
            m_depth = 1;
            return true;
        }
        void unlock()
        {
            if (--m_depth)
                return;
            m_owner.store(std::thread::id(), std::memory_order_relaxed);
            m_mutex.unlock();
        }

    private:
        std::mutex m_mutex;
        std::atomic<std::thread::id> m_owner{};
        unsigned m_depth = 0;
    };
#else
    using RecursiveMutex = std::recursive_mutex;
#endif
}
