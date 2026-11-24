#pragma once
// Early D3D12 device hooks: track every buffer view written into CBV/SRV/UAV descriptor heaps,
// so later code can turn a descriptor (bindless slot or root-table entry) back into a resource
// and GPU virtual address.

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace rt
{
    struct BufferView
    {
        ID3D12Resource*           resource = nullptr;  // not AddRef'd: valid while the engine keeps it alive
        D3D12_GPU_VIRTUAL_ADDRESS gpuva = 0;           // address of the view's first byte
        std::uint64_t             sizeBytes = 0;
        std::uint32_t             stride = 0;          // structured stride, 0 for raw/typed
        bool                      raw = false;
        bool                      uav = false;
        bool                      cbv = false;                 // constant buffer view (resource is null)
        // 0.2.22: texture views (SRV/UAV that are not buffers). `resource` stays null for them so buffer lookups
        // never mistake a texture for a buffer.
        ID3D12Resource*           texture = nullptr;
        DXGI_FORMAT               format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t             viewDim = 0;               // D3D12_SRV_DIMENSION / D3D12_UAV_DIMENSION value
        // 0.2.43: the texture's shape, recorded when the view is written (the texture is alive then). Use these instead
        // of texture->GetDesc(): a table can keep stale entries whose texture has since been freed.
        std::uint32_t             texWidth = 0, texHeight = 0;
        std::uint16_t             texDepthOrArray = 0;
        std::uint8_t              texMips = 0;
        std::uint8_t              texDim = 0;                // D3D12_RESOURCE_DIMENSION
        DXGI_FORMAT               texFormat = DXGI_FORMAT_UNKNOWN;

        bool valid() const { return resource != nullptr || cbv; }
    };

    // Patch Starfield's import of sl.interposer!D3D12CreateDevice. Call from SFSE plugin load,
    // before the renderer creates its device.
    bool InstallDeviceCreationHook();

    // True once a device has been created and its descriptor hooks are in place.
    bool DeviceHooksReady();

    // 0.1.31: DRED (Device Removed Extended Data). Call EnableDred before the game creates its device; then
    // CheckDeviceRemoved once per frame logs the removal reason, the faulting GPU address with the allocations
    // around it, and where each command list stopped. Returns true once the device is gone.
    void EnableDred(bool on);

    // 0.2.13: the upload-heap buffer containing a GPU address (offset within it), or nullptr. Only meaningful for an
    // address that is in use right now (e.g. a CBV just bound): the newest registered resource at an address wins.
    ID3D12Resource* FindUploadBuffer(D3D12_GPU_VIRTUAL_ADDRESS va, std::uint64_t& offset);
    std::uint64_t   UploadBufferCount();

    // 0.2.15: called for every CBV the engine creates (GPU address, size), right after the view is written.
    using CbvCallback = void (*)(D3D12_GPU_VIRTUAL_ADDRESS va, UINT size);
    void SetCbvCallback(CbvCallback cb);
    // 0.1.32: takes the renderer's live device (the first device the game creates is a short-lived one).
    bool CheckDeviceRemoved(ID3D12Device* device);
    // 0.2.33: logs local video memory use against the OS budget (and the peak so far). `when` labels the line.
    void LogVram(ID3D12Device* device, const char* when);

    // Look up the buffer view at (shader-visible GPU table base + offset descriptors).
    bool LookupGpuTable(D3D12_GPU_DESCRIPTOR_HANDLE tableBase, std::uint32_t offset, BufferView& out);
    // 0.2.24: like LookupGpuTable, but also returns texture views (LookupGpuTable returns buffers and CBVs only).
    bool LookupGpuTableAny(D3D12_GPU_DESCRIPTOR_HANDLE tableBase, std::uint32_t offset, BufferView& out);

    // Look up the buffer view at an absolute index in the shader-visible heap containing `anyHandle`.
    bool LookupHeapIndex(D3D12_GPU_DESCRIPTOR_HANDLE anyHandle, std::uint32_t index, BufferView& out);
    // 0.5.14: like LookupHeapIndex for a buffer, plus a reference that keeps it alive (taken from the slot's own reference,
    // never from a pointer the game may already have freed).
    // 0.5.14: buffers currently held through shader-visible descriptor slots (for the log).
    std::int64_t HeldBufferCount();
    bool LookupHeapIndexRef(D3D12_GPU_DESCRIPTOR_HANDLE anyHandle, std::uint32_t index, BufferView& out,
        Microsoft::WRL::ComPtr<ID3D12Resource>& keep);

    // Index of `handle` inside its shader-visible heap, or UINT32_MAX.
    std::uint32_t HeapIndexOf(D3D12_GPU_DESCRIPTOR_HANDLE handle);

    // 0.2.39: CPU handle of (shader-visible GPU table base + offset descriptors), for writing one descriptor in place.
    bool GpuTableToCpu(D3D12_GPU_DESCRIPTOR_HANDLE tableBase, std::uint32_t offset, D3D12_CPU_DESCRIPTOR_HANDLE& out);

    // Counts of buffer views tracked in [first, first+count) of the heap containing `anyHandle`.
    std::uint32_t CountBufferViews(D3D12_GPU_DESCRIPTOR_HANDLE anyHandle, std::uint32_t first, std::uint32_t count);

    void LogDeviceHookStats();
    void LogHeaps(std::uint64_t probeGpu);  // 0.2.53: every tracked CBV/SRV/UAV heap, and which one contains probeGpu

    // 0.2.22: render target views by CPU handle (resource and view format), for the reflection survey.
    bool LookupRtv(SIZE_T cpu, ID3D12Resource*& resource, DXGI_FORMAT& format);
    UINT RtvIncrement();
}
