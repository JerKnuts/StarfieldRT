// 0.2.1: see GpuTimer.h.

#include <SFSE/SFSE.h>  // CommonLibSF must come before any Windows headers

#include "GpuTimer.h"

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace rt::timer
{
    namespace
    {
        constexpr std::uint32_t kRing = 4;       // frames in flight we tolerate before reading a slot back
        constexpr std::uint64_t kWindow = 600;   // frames per log line (fewer if the RT state changes)

        std::atomic<bool>          g_enabled{ true };
        std::mutex                 g_lock;
        ComPtr<ID3D12QueryHeap>    g_heap;
        ComPtr<ID3D12Resource>     g_readback;
        const std::uint64_t*       g_mapped = nullptr;
        double                     g_ticksPerMs = 0.0;
        bool                       g_initFailed = false;

        struct Slot
        {
            std::uint32_t written = 0;  // bit per Mark stamped
            bool          pending = false;
            double        cpuMs = 0.0;
            std::uint32_t gen = 0;      // 0.2.2: measurement window the frame belongs to
        };
        std::uint32_t              g_gen = 1;
        std::array<Slot, kRing>    g_slots{};
        std::uint32_t              g_current = UINT32_MAX;  // slot being recorded this frame

        // Window accumulators.
        double        g_gpu[kMarkCount]{};  // per stage (index = end mark), ms
        std::uint32_t g_gpuN[kMarkCount]{};
        double        g_cpuSum = 0.0;
        std::uint32_t g_cpuN = 0;
        double        g_frameSum = 0.0;
        std::uint64_t g_frames = 0;
        LARGE_INTEGER g_lastTick{};
        int           g_lastMode = -1;
        bool          g_lastSoft = false, g_lastAlpha = false;

        bool Init(ID3D12Device* device)
        {
            if (g_heap) return true;
            if (g_initFailed) return false;
            g_initFailed = true;
            // Timestamp frequency comes from a queue; a throwaway direct queue reports the adapter's clock.
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> queue;
            UINT64 freq = 0;
            if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(queue->GetTimestampFrequency(&freq)) || !freq) {
                REX::WARN("Starfield RT: profiler disabled (no timestamp frequency)");
                return false;
            }
            g_ticksPerMs = static_cast<double>(freq) / 1000.0;
            D3D12_QUERY_HEAP_DESC hd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kRing * kMarkCount, 0 };
            if (FAILED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&g_heap)))) {
                REX::WARN("Starfield RT: profiler disabled (query heap)");
                return false;
            }
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = kRing * kMarkCount * sizeof(std::uint64_t);
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* p = nullptr;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&g_readback))) ||
                FAILED(g_readback->Map(0, nullptr, &p)) || !p) {
                g_heap.Reset();
                REX::WARN("Starfield RT: profiler disabled (readback buffer)");
                return false;
            }
            g_mapped = static_cast<const std::uint64_t*>(p);
            g_initFailed = false;
            REX::INFO("Starfield RT: profiler ready (GPU timestamps at {:.0f} MHz)", g_ticksPerMs / 1000.0);
            return true;
        }

        // Read a finished slot (kRing frames old) into the window sums.
        void Collect(std::uint32_t s)
        {
            Slot& slot = g_slots[s];
            if (!slot.pending) return;
            slot.pending = false;
            if (slot.gen != g_gen) return;  // 0.2.2: recorded before the last toggle; don't mix states
            const std::uint64_t* t = g_mapped + s * kMarkCount;
            for (std::uint32_t m = 1; m < kMarkCount; ++m) {
                if (!(slot.written & (1u << m)) || !(slot.written & (1u << (m - 1)))) continue;
                if (t[m] < t[m - 1]) continue;
                const double ms = static_cast<double>(t[m] - t[m - 1]) / g_ticksPerMs;
                if (ms > 1000.0) continue;  // garbage
                g_gpu[m] += ms;
                ++g_gpuN[m];
            }
            g_cpuSum += slot.cpuMs;
            ++g_cpuN;
        }

        void ResetWindow()
        {
            ++g_gen;
            for (auto& v : g_gpu) v = 0.0;
            for (auto& n : g_gpuN) n = 0;
            g_cpuSum = 0.0;
            g_cpuN = 0;
            g_frameSum = 0.0;
            g_frames = 0;
        }

        double Avg(std::uint32_t m) { return g_gpuN[m] ? g_gpu[m] / g_gpuN[m] : 0.0; }

        void LogWindow()
        {
            if (!g_frames) return;
            const double frameMs = g_frameSum / g_frames;
            const double blas = Avg(kAfterBlas), tlas = Avg(kAfterTlas), shadow = Avg(kAfterShadow), filter = Avg(kAfterFilter);
            REX::INFO("Starfield RT: PERF RT mode {} soft {} alpha {} ({} frames): frame {:.2f} ms ({:.0f} fps) | plugin GPU: "
                      "BLAS {:.2f} + TLAS {:.2f} + shadow rays {:.2f} + soft filter {:.2f} = {:.2f} ms | plugin CPU {:.2f} ms",
                g_lastMode, g_lastSoft ? "on" : "off", g_lastAlpha ? "on" : "off", g_frames, frameMs,
                frameMs > 0.0 ? 1000.0 / frameMs : 0.0, blas, tlas, shadow, filter, blas + tlas + shadow + filter,
                g_cpuN ? g_cpuSum / g_cpuN : 0.0);
        }
    }

    void SetEnabled(bool on) { g_enabled = on; }
    bool Enabled() { return g_enabled.load(); }

    void BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::uint64_t frame)
    {
        if (!g_enabled.load() || !device || !list) return;
        std::lock_guard lock(g_lock);
        if (!Init(device)) return;
        const std::uint32_t s = static_cast<std::uint32_t>(frame % kRing);
        Collect(s);  // this slot was last used kRing frames ago
        g_slots[s] = Slot{};
        g_slots[s].gen = g_gen;
        g_current = s;
        list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * kMarkCount + kStart);
        g_slots[s].written |= 1u << kStart;
    }

    void Stamp(ID3D12GraphicsCommandList* list, Mark m)
    {
        if (!g_enabled.load() || !list) return;
        std::lock_guard lock(g_lock);
        if (g_current == UINT32_MAX || !g_heap) return;
        list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_current * kMarkCount + m);
        g_slots[g_current].written |= 1u << m;
    }

    void EndFrame(ID3D12GraphicsCommandList* list, double cpuMs)
    {
        if (!list) return;
        std::lock_guard lock(g_lock);
        if (g_current == UINT32_MAX || !g_heap) return;
        const std::uint32_t s = g_current;
        list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * kMarkCount, kMarkCount, g_readback.Get(),
            s * kMarkCount * sizeof(std::uint64_t));
        g_slots[s].pending = true;
        g_slots[s].cpuMs = cpuMs;
        g_current = UINT32_MAX;
    }

    void FrameTick(std::uint64_t, int sunMode, bool soft, bool alpha)
    {
        if (!g_enabled.load()) return;
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        std::lock_guard lock(g_lock);
        const bool changed = sunMode != g_lastMode || soft != g_lastSoft || alpha != g_lastAlpha;
        if (changed) {
            if (g_frames >= 60) LogWindow();  // what we have for the previous state
            ResetWindow();
            g_lastMode = sunMode;
            g_lastSoft = soft;
            g_lastAlpha = alpha;
            g_lastTick = now;
            return;
        }
        if (g_lastTick.QuadPart) {
            const double ms = static_cast<double>(now.QuadPart - g_lastTick.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
            if (ms < 500.0) {  // skip pauses and load screens
                g_frameSum += ms;
                ++g_frames;
            }
        }
        g_lastTick = now;
        if (g_frames >= kWindow) {
            LogWindow();
            ResetWindow();
        }
    }
}
