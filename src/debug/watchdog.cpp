#include "watchdog.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace vcs
{
    void startWatchdog(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_WATCHDOG");
        if (!env || !*env)
            return;
        const double seconds = std::atof(env) > 0.0 ? std::atof(env) : 1.0;

        std::thread([&runtime, seconds]()
                    {
            const auto period = std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
            uint64_t lastCycle = 0;
            while (!runtime.isStopRequested())
            {
                std::this_thread::sleep_for(period);
                const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
                std::fprintf(stderr, "[watchdog] cycle=%llu (+%llu) running=%d pc=%08x ra=%08x sp=%08x\n",
                             static_cast<unsigned long long>(snap.eeCycle),
                             static_cast<unsigned long long>(snap.eeCycle - lastCycle),
                             snap.runningThreadId,
                             runtime.m_debugPc.load(std::memory_order_relaxed),
                             runtime.m_debugRa.load(std::memory_order_relaxed),
                             runtime.m_debugSp.load(std::memory_order_relaxed));
                lastCycle = snap.eeCycle;
                const ps2x::iop::DebugSnapshot iop = runtime.iopDebugSnapshot();
                std::fprintf(stderr, "[watchdog] iop cycles=%llu instructions=%llu modules=%u threads=%u rpcServers=%u\n",
                             static_cast<unsigned long long>(iop.emulatorCycles),
                             static_cast<unsigned long long>(iop.emulatorInstructions),
                             iop.emulatorLoadedModules, iop.emulatorThreads, iop.emulatorRpcServers);
                for (const std::string &d : iop.diagnostics)
                    std::fprintf(stderr, "[watchdog] iop: %s\n", d.c_str());
                for (const EeThreadSnapshot &t : snap.threads)
                {
                    std::fprintf(stderr, "[watchdog]   thread %d prio=%d status=%d wait=%d/%d pc=%08x ra=%08x entry=%08x\n",
                                 t.id, t.currentPriority, static_cast<int>(t.status),
                                 static_cast<int>(t.waitReason), t.waitId, t.pc, t.ra, t.entry);
                }
                std::fflush(stderr);
            } })
            .detach();
    }
}
