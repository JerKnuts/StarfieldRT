#pragma once
// 0.2.1: GPU/CPU profiler for the plugin's per-frame RT work. GPU timestamps are written on the
// command list between the stages below, resolved into a readback ring and averaged without stalling.

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>

namespace rt::timer
{
    enum Mark : std::uint32_t
    {
        kStart = 0,      // before any plugin work on the list
        kAfterBlas,      // geometry copies + BLAS builds
        kAfterTlas,      // instance descriptors + TLAS build
        kAfterShadow,    // sun shadow rays (pass 1)
        kAfterFilter,    // soft shadow filter (pass 2)
        kMarkCount
    };

    void SetEnabled(bool on);
    bool Enabled();

    // Call once per frame on the list the plugin records into, before its first RT work.
    void BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::uint64_t frame);
    // Timestamp `m` on the current frame's list (no-op if the frame wasn't begun).
    void Stamp(ID3D12GraphicsCommandList* list, Mark m);
    // Resolve the frame's timestamps (call after the last plugin work on the list).
    void EndFrame(ID3D12GraphicsCommandList* list, double cpuMs);

    // Frame tick (once per frame, any thread): accumulates frame time and logs averages every ~600 frames.
    void FrameTick(std::uint64_t frame, int sunMode, bool soft, bool alpha);
}
