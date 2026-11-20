#include "hardware.h"

#include <algorithm>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#else
#include <unistd.h>
#endif

namespace launcher
{
    namespace
    {
#ifdef _WIN32
        std::string narrow(const wchar_t *w)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1)
                return {};
            std::string s(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }

        // the card the game takes (vcs.exe asks for the high-performance one first, as here)
        void probeGpu(HardwareInfo &hw)
        {
            using Microsoft::WRL::ComPtr;
            ComPtr<IDXGIFactory1> factory1;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))))
                return;
            ComPtr<IDXGIFactory6> factory6;
            factory1.As(&factory6);
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> adapter;
                const HRESULT hr = factory6
                                       ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                              IID_PPV_ARGS(&adapter))
                                       : factory1->EnumAdapters1(i, &adapter);
                if (FAILED(hr))
                    return;
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                    continue;
                hw.gpuName = narrow(d.Description);
                hw.vramMb = d.DedicatedVideoMemory >> 20;
                // an integrated GPU reports a small carve-out (or none) and lives on the system's memory
                hw.integrated = hw.vramMb < 512;
                return;
            }
        }
#endif

        uint64_t physicalMemoryMb()
        {
#ifdef _WIN32
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            return GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys >> 20 : 0;
#else
            const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
            return pages > 0 && page > 0 ? (static_cast<uint64_t>(pages) * static_cast<uint64_t>(page)) >> 20 : 0;
#endif
        }
    }

    HardwareInfo probeHardware()
    {
        HardwareInfo hw;
#ifdef _WIN32
        probeGpu(hw);
#endif
        hw.threads = static_cast<int>(std::thread::hardware_concurrency());
#if defined(__x86_64__) || defined(_M_X64)
        hw.avx2 = __builtin_cpu_supports("avx2");
#endif
        hw.ramMb = physicalMemoryMb();
        return hw;
    }

    GraphicsPreset recommendPreset(const HardwareInfo &hw, std::string &why)
    {
        // each part's ceiling: 0 Original (PS2), 1 Low, 2 Medium, 3 High. The heavy settings are the internal
        // resolution and MSAA (the graphics card) and the detailed city and population (the game thread: the
        // processor), so each preset is taken one step below what the part could just about hold.
        int gpu = 2; // no card read (Linux): Medium
        if (!hw.gpuName.empty())
            gpu = hw.integrated ? 1 : hw.vramMb < 3072 ? 1 : hw.vramMb < 6144 ? 2 : 3;
        const int cpu = hw.threads >= 12 && hw.avx2 ? 3 : hw.threads >= 8 ? 2 : hw.threads >= 4 ? 1 : 0;
        // 8 GB reports a little under 8192, so the thresholds sit below the sizes they mean
        const int ram = hw.ramMb == 0 ? 2 : hw.ramMb < 3500 ? 0 : hw.ramMb < 7000 ? 1 : hw.ramMb < 15000 ? 2 : 3;
        const int tier = std::min({gpu, cpu, ram});

        why = hw.gpuName.empty() ? std::string("GPU ?") : hw.gpuName;
        if (!hw.gpuName.empty() && !hw.integrated)
            why += " (" + std::to_string((hw.vramMb + 512) >> 10) + " GB)";
        why += ", " + std::to_string(hw.threads) + " threads" + (hw.avx2 ? " (AVX2)" : "") + ", " +
               std::to_string((hw.ramMb + 512) >> 10) + " GB RAM";

        static constexpr GraphicsPreset kTiers[] = {GraphicsPreset::OriginalPS2, GraphicsPreset::Low,
                                                    GraphicsPreset::Medium, GraphicsPreset::High};
        return kTiers[tier];
    }
}
