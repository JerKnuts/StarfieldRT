// Early D3D12 device hooks (see DeviceHooks.h).
// Starfield imports D3D12CreateDevice from NVIDIA Streamline (sl.interposer.dll). We patch that IAT
// entry at plugin load; when the device is created we hook its descriptor-writing methods on the
// (Streamline proxy) vtable and mirror every buffer view into our own per-heap tables.

#include <SFSE/SFSE.h>  // CommonLibSF must come before any Windows headers

#include "DeviceHooks.h"

#include <dxgi1_4.h>
#include <wrl/client.h>
#include <string>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace rt
{
    namespace
    {
        constexpr std::uintptr_t kIatD3D12CreateDeviceRva = 0x3A40BB0;  // Starfield 1.16.244

        // ID3D12Device vtable indices.
        enum : std::size_t
        {
            kCreateDescriptorHeap = 14,
            kCreateConstantBufferView = 17,
            kCreateShaderResourceView = 18,
            kCreateUnorderedAccessView = 19,
            kCreateRenderTargetView = 20,  // 0.2.22
            kCopyDescriptors = 23,
            kCopyDescriptorsSimple = 24,
            kCreateCommittedResource = 27,   // 0.2.13: upload-buffer registry
            kCreatePlacedResource = 29,
            kCreateCommittedResource1 = 53,  // ID3D12Device4
            kCreateCommittedResource2 = 69,  // 0.4.2: ID3D12Device8
            kCreatePlacedResource1 = 70,
            kCreateCommittedResource3 = 76,  // 0.4.2: ID3D12Device10
            kCreatePlacedResource2 = 77,
            kDeviceSlots = 78,
        };

        // 0.2.13: upload-heap buffers by GPU address range, so a CBV (which only carries a GPU address) can be traced
        // back to its resource and written from the CPU. Entries are weak: when a new resource covers an address
        // range, older overlapping entries are dropped first, so the newest (live) resource at an address always wins.
        struct UploadBuf { ID3D12Resource* res; std::uint64_t size; };
        std::mutex                                  g_uploadLock;
        std::map<D3D12_GPU_VIRTUAL_ADDRESS, UploadBuf> g_uploads;
        std::atomic<std::uint64_t>                  g_uploadCount{ 0 };

        // 0.4.2: every CPU-writable heap counts, not just UPLOAD: custom heaps with CPU access and GPU-upload (ReBAR,
        // heap type 5) heaps too. The game's lamp lists weren't found with UPLOAD alone.
        bool CpuWritable(const D3D12_HEAP_PROPERTIES& hp)
        {
            if (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == static_cast<D3D12_HEAP_TYPE>(5)) return true;
            return hp.Type == D3D12_HEAP_TYPE_CUSTOM && (hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
                                                         hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
        }
        // Resource interfaces whose pointer is the resource itself (the newer ones derive from ID3D12Resource).
        bool ResourceIid(REFIID riid)
        {
            return riid == __uuidof(ID3D12Resource) || riid == __uuidof(ID3D12Resource1) || riid == __uuidof(ID3D12Resource2);
        }

        void RegisterUpload(ID3D12Resource* r, D3D12_RESOURCE_DIMENSION dim, std::uint64_t width)
        {
            if (!r || dim != D3D12_RESOURCE_DIMENSION_BUFFER) return;
            const D3D12_GPU_VIRTUAL_ADDRESS va = r->GetGPUVirtualAddress();
            if (!va) return;
            const std::uint64_t size = width;
            std::lock_guard lock(g_uploadLock);
            auto it = g_uploads.lower_bound(va);
            if (it != g_uploads.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size > va) it = prev;
            }
            while (it != g_uploads.end() && it->first < va + size) it = g_uploads.erase(it);
            g_uploads.emplace(va, UploadBuf{ r, size });
            g_uploadCount.fetch_add(1);
        }

        // 0.5.14: our own reference to each buffer recorded in a shader-visible heap slot, swapped under this lock and
        // released when the slot is overwritten. Before, the BLAS code AddRef'd the raw pointer from a slot whose buffer
        // the game might already have freed (streamed vertex buffers during a load): a write into freed memory that
        // later crashed the game's UI code (Scaleform) on save loads.
        std::mutex g_ownLock;
        std::atomic<std::int64_t> g_ownedCount{ 0 };

        struct Heap
        {
            SIZE_T                      cpuStart = 0;
            UINT64                      gpuStart = 0;  // 0 when not shader visible
            UINT                        count = 0;
            std::unique_ptr<BufferView[]> views;
            std::unique_ptr<ID3D12Resource*[]> owned;  // 0.5.14: shader-visible heaps only
            ~Heap()
            {
                if (!owned) return;
                std::lock_guard lock(g_ownLock);
                for (UINT i = 0; i < count; ++i) {
                    if (owned[i]) {
                        owned[i]->Release();
                        g_ownedCount.fetch_sub(1, std::memory_order_relaxed);
                    }
                }
            }
        };

        // 0.5.14: point a slot's own reference at `r`: a buffer the game is passing to us right now (alive), or null.
        void SetOwned(Heap* h, UINT index, ID3D12Resource* r)
        {
            if (!h || !h->owned || index >= h->count) return;
            if (h->owned[index] == r) return;  // the same buffer copied in again (most copies): nothing to swap
            std::lock_guard lock(g_ownLock);
            ID3D12Resource* old = h->owned[index];
            if (old == r) return;
            if (r) r->AddRef();
            h->owned[index] = r;
            if (old) old->Release();
            g_ownedCount.fetch_add((r ? 1 : 0) - (old ? 1 : 0), std::memory_order_relaxed);
        }

        // 0.5.17: a descriptor copy takes its reference from the source slot's own reference (read under the lock), never
        // from the source's raw pointer: games copy whole blocks of descriptors, stale ones included, and 0.5.14 AddRef'd
        // those raw pointers (a write into freed memory again, at startup or on loads).
        void SetOwnedFrom(Heap* h, UINT index, Heap* sh, UINT sindex)
        {
            if (!h || !h->owned || index >= h->count) return;
            const bool haveSrc = sh && sh->owned && sindex < sh->count;
            if (h->owned[index] == (haveSrc ? sh->owned[sindex] : nullptr)) return;  // unchanged (most copies)
            std::lock_guard lock(g_ownLock);
            ID3D12Resource* r = haveSrc ? sh->owned[sindex] : nullptr;
            ID3D12Resource* old = h->owned[index];
            if (old == r) return;
            if (r) r->AddRef();  // alive: the source slot holds our reference to it
            h->owned[index] = r;
            if (old) old->Release();
            g_ownedCount.fetch_add((r ? 1 : 0) - (old ? 1 : 0), std::memory_order_relaxed);
        }

        std::shared_mutex                  g_heapLock;
        std::vector<std::unique_ptr<Heap>> g_heaps;        // CBV/SRV/UAV heaps only
        UINT                               g_increment = 0;
        UINT                               g_rtvIncrement = 0;
        struct RtvEntry { ID3D12Resource* res; DXGI_FORMAT format; };
        std::mutex                         g_rtvLock;
        std::unordered_map<SIZE_T, RtvEntry> g_rtvs;
        std::atomic<bool>                  g_ready{ false };
        std::array<void*, kDeviceSlots>    g_orig{};
        void*                              g_origCreateDevice = nullptr;
        std::atomic<std::uint64_t>         g_bufferViewsSeen{ 0 }, g_copies{ 0 };

        bool PatchPointer(void** slot, void* value, void** original)
        {
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                return false;
            }
            if (original) {
                *original = *slot;
            }
            InterlockedExchangePointer(slot, value);
            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }

        // Caller holds g_heapLock (shared is enough for slot writes: slots are independent).
        BufferView* SlotForCpu(SIZE_T cpu, Heap** heapOut = nullptr, UINT* indexOut = nullptr)
        {
            for (auto& h : g_heaps) {
                if (cpu >= h->cpuStart && cpu < h->cpuStart + SIZE_T(h->count) * g_increment) {
                    const UINT i = static_cast<UINT>((cpu - h->cpuStart) / g_increment);
                    if (heapOut) *heapOut = h.get();
                    if (indexOut) *indexOut = i;
                    return &h->views[i];
                }
            }
            return nullptr;
        }

        Heap* HeapForGpu(UINT64 gpu)
        {
            for (auto& h : g_heaps) {
                if (h->gpuStart && gpu >= h->gpuStart && gpu < h->gpuStart + UINT64(h->count) * g_increment) {
                    return h.get();
                }
            }
            return nullptr;
        }

        std::uint32_t FormatBytes(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R16_UINT:
            case DXGI_FORMAT_R16_SINT:
            case DXGI_FORMAT_R16_UNORM:
            case DXGI_FORMAT_R16_FLOAT:
                return 2;
            case DXGI_FORMAT_R8_UINT:
            case DXGI_FORMAT_R8_UNORM:
                return 1;
            case DXGI_FORMAT_R32G32_UINT:
            case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            case DXGI_FORMAT_R32G32B32A32_UINT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return 16;
            default:
                return 4;
            }
        }

        // ---- device hooks ------------------------------------------------------------------

        using CreateHeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreateDescriptorHeap(ID3D12Device* d, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreateHeapFn>(g_orig[kCreateDescriptorHeap])(d, desc, riid, out);
            if (SUCCEEDED(hr) && desc && out && *out && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                auto* heap = static_cast<ID3D12DescriptorHeap*>(*out);
                auto h = std::make_unique<Heap>();
                h->cpuStart = heap->GetCPUDescriptorHandleForHeapStart().ptr;
                h->gpuStart = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? heap->GetGPUDescriptorHandleForHeapStart().ptr : 0;
                h->count = desc->NumDescriptors;
                h->views = std::make_unique<BufferView[]>(desc->NumDescriptors);
                // 0.5.14 shader-visible heaps; 0.5.17 every CBV/SRV/UAV heap (copies take their reference from the source slot)
                h->owned = std::make_unique<ID3D12Resource*[]>(desc->NumDescriptors);  // zeroed
                std::unique_lock lock(g_heapLock);
                // Drop heaps whose address range the new heap reuses (the old one was released).
                const SIZE_T end = h->cpuStart + SIZE_T(h->count) * g_increment;
                std::erase_if(g_heaps, [&](const std::unique_ptr<Heap>& o) {
                    return o->cpuStart < end && h->cpuStart < o->cpuStart + SIZE_T(o->count) * g_increment;
                });
                if (h->gpuStart && h->count >= 4096) {
                    REX::INFO("Starfield RT: shader-visible CBV/SRV/UAV heap created: {} descriptors", h->count);
                }
                g_heaps.push_back(std::move(h));
            }
            return hr;
        }

        void RecordTextureShape(BufferView& v, ID3D12Resource* r)  // 0.2.43
        {
            const D3D12_RESOURCE_DESC d = r->GetDesc();
            v.texWidth = static_cast<std::uint32_t>(d.Width);
            v.texHeight = d.Height;
            v.texDepthOrArray = d.DepthOrArraySize;
            v.texMips = static_cast<std::uint8_t>((std::min)(d.MipLevels, UINT16(255)));
            v.texDim = static_cast<std::uint8_t>(d.Dimension);
            v.texFormat = d.Format;
        }

        void BlankIfProbe(ID3D12Device* d, Heap* heap, const BufferView* v, SIZE_T cpu);  // 0.5.130, below
        using CreateSrvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
        void STDMETHODCALLTYPE H_CreateShaderResourceView(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dst)
        {
            reinterpret_cast<CreateSrvFn>(g_orig[kCreateShaderResourceView])(d, r, desc, dst);
            std::shared_lock lock(g_heapLock);
            Heap* heap = nullptr;
            UINT index = 0;
            BufferView* slot = SlotForCpu(dst.ptr, &heap, &index);
            if (!slot) {
                return;
            }
            SetOwned(heap, index, (r && desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) ? r : nullptr);  // 0.5.14
            if (r && desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
                const auto& b = desc->Buffer;
                const bool raw = (b.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
                const std::uint32_t elem = b.StructureByteStride ? b.StructureByteStride : (raw ? 4 : FormatBytes(desc->Format));
                *slot = { r, r->GetGPUVirtualAddress() + b.FirstElement * elem, std::uint64_t(b.NumElements) * elem,
                    b.StructureByteStride, raw, false };
                g_bufferViewsSeen.fetch_add(1, std::memory_order_relaxed);
            } else {
                *slot = {};
                if (r) {
                    slot->texture = r;
                    RecordTextureShape(*slot, r);
                    slot->format = desc ? desc->Format : slot->texFormat;
                    slot->viewDim = desc ? static_cast<std::uint32_t>(desc->ViewDimension) : 4;  // TEXTURE2D
                    BlankIfProbe(d, heap, slot, dst.ptr);  // 0.5.130
                }
            }
        }

        using CreateUavFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
        void STDMETHODCALLTYPE H_CreateUnorderedAccessView(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dst)
        {
            reinterpret_cast<CreateUavFn>(g_orig[kCreateUnorderedAccessView])(d, r, c, desc, dst);
            std::shared_lock lock(g_heapLock);
            Heap* heap = nullptr;
            UINT index = 0;
            BufferView* slot = SlotForCpu(dst.ptr, &heap, &index);
            if (!slot) {
                return;
            }
            SetOwned(heap, index, (r && desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) ? r : nullptr);  // 0.5.14
            if (r && desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
                const auto& b = desc->Buffer;
                const bool raw = (b.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
                const std::uint32_t elem = b.StructureByteStride ? b.StructureByteStride : (raw ? 4 : FormatBytes(desc->Format));
                *slot = { r, r->GetGPUVirtualAddress() + b.FirstElement * elem, std::uint64_t(b.NumElements) * elem,
                    b.StructureByteStride, raw, true };
            } else {
                *slot = {};
                if (r) {
                    slot->texture = r;
                    slot->uav = true;
                    RecordTextureShape(*slot, r);
                    slot->format = desc ? desc->Format : slot->texFormat;
                    slot->viewDim = desc ? static_cast<std::uint32_t>(desc->ViewDimension) : 4;
                }
            }
        }

        using CreateCbvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
        std::atomic<CbvCallback> g_cbvCallback{ nullptr };

        void STDMETHODCALLTYPE H_CreateConstantBufferView(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dst)
        {
            reinterpret_cast<CreateCbvFn>(g_orig[kCreateConstantBufferView])(d, desc, dst);
            if (auto cb = g_cbvCallback.load(std::memory_order_relaxed); cb && desc && desc->BufferLocation) {
                cb(desc->BufferLocation, desc->SizeInBytes);
            }
            std::shared_lock lock(g_heapLock);
            Heap* heap = nullptr;
            UINT index = 0;
            if (BufferView* slot = SlotForCpu(dst.ptr, &heap, &index)) {
                SetOwned(heap, index, nullptr);  // 0.5.14
                *slot = {};
                if (desc && desc->BufferLocation) {
                    slot->gpuva = desc->BufferLocation;
                    slot->sizeBytes = desc->SizeInBytes;
                    slot->cbv = true;
                }
            }
        }

        // 0.5.130: cubemap blanking (see DeviceHooks.h).
        std::atomic<ID3D12Resource*> g_blankProbe{ nullptr };
        std::atomic<ID3D12Resource*> g_blankBlack{ nullptr };
        std::atomic<DXGI_FORMAT>     g_blankFormat{ DXGI_FORMAT_UNKNOWN };
        std::atomic<UINT>            g_blankSlices{ 0 };
        std::atomic<bool>            g_blankOn{ false };
        std::atomic<std::uint64_t>   g_blankCount{ 0 };
        thread_local bool            t_noBlank = false;  // 0.5.131
        void BlankIfProbe(ID3D12Device* d, Heap* heap, const BufferView* v, SIZE_T cpu)
        {
            if (t_noBlank || !heap || !heap->gpuStart || !v || v->uav || !v->texture || !g_blankOn.load(std::memory_order_relaxed)) return;
            ID3D12Resource* probe = g_blankProbe.load(std::memory_order_relaxed);
            ID3D12Resource* black = g_blankBlack.load(std::memory_order_relaxed);
            if (!probe || !black || v->texture != probe || v->viewDim != D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) return;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = g_blankFormat.load(std::memory_order_relaxed);
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            sd.TextureCubeArray.MipLevels = 1;
            sd.TextureCubeArray.NumCubes = g_blankSlices.load(std::memory_order_relaxed) / 6;
            // The game's views may cover fewer cubes than the array; ours covers them all (indices stay in range).
            reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                D3D12_CPU_DESCRIPTOR_HANDLE)>(g_orig[kCreateShaderResourceView])(d, black, &sd, D3D12_CPU_DESCRIPTOR_HANDLE{ cpu });
            g_blankCount.fetch_add(1, std::memory_order_relaxed);
        }

        void CopyRange(ID3D12Device* d, SIZE_T dst, SIZE_T src, UINT n)
        {
            for (UINT i = 0; i < n; ++i) {
                Heap* sheap = nullptr;
                UINT sindex = 0;
                BufferView* s = SlotForCpu(src + SIZE_T(i) * g_increment, &sheap, &sindex);
                Heap* heap = nullptr;
                UINT index = 0;
                BufferView* t = SlotForCpu(dst + SIZE_T(i) * g_increment, &heap, &index);
                if (t) {
                    SetOwnedFrom(heap, index, s ? sheap : nullptr, sindex);  // 0.5.17
                    *t = s ? *s : BufferView{};
                    BlankIfProbe(d, heap, t, dst + SIZE_T(i) * g_increment);  // 0.5.130
                }
            }
        }

        using CopySimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
        void STDMETHODCALLTYPE H_CopyDescriptorsSimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
        {
            reinterpret_cast<CopySimpleFn>(g_orig[kCopyDescriptorsSimple])(d, n, dst, src, type);
            if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                std::shared_lock lock(g_heapLock);
                CopyRange(d, dst.ptr, src.ptr, n);
                g_copies.fetch_add(1, std::memory_order_relaxed);
            }
        }

        using CopyFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
            const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
        void STDMETHODCALLTYPE H_CopyDescriptors(ID3D12Device* d, UINT nDst, const D3D12_CPU_DESCRIPTOR_HANDLE* dstStarts, const UINT* dstSizes,
            UINT nSrc, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts, const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
        {
            reinterpret_cast<CopyFn>(g_orig[kCopyDescriptors])(d, nDst, dstStarts, dstSizes, nSrc, srcStarts, srcSizes, type);
            if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !dstStarts || !srcStarts) {
                return;
            }
            std::shared_lock lock(g_heapLock);
            // Walk both range lists in lockstep, one descriptor at a time (sizes default to 1 when null).
            UINT di = 0, si = 0, doff = 0, soff = 0;
            while (di < nDst && si < nSrc) {
                const UINT dsz = dstSizes ? dstSizes[di] : 1;
                const UINT ssz = srcSizes ? srcSizes[si] : 1;
                const UINT run = (std::min)(dsz - doff, ssz - soff);
                CopyRange(d, dstStarts[di].ptr + SIZE_T(doff) * g_increment, srcStarts[si].ptr + SIZE_T(soff) * g_increment, run);
                doff += run;
                soff += run;
                if (doff == dsz) { ++di; doff = 0; }
                if (soff == ssz) { ++si; soff = 0; }
            }
            g_copies.fetch_add(1, std::memory_order_relaxed);
        }

        using CreateCommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
            const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreateCommittedResource(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
            const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreateCommittedFn>(g_orig[kCreateCommittedResource])(d, hp, hf, desc, st, cv, riid, out);
            if (SUCCEEDED(hr) && out && *out && hp && desc && CpuWritable(*hp) && ResourceIid(riid)) {
                RegisterUpload(static_cast<ID3D12Resource*>(*out), desc->Dimension, desc->Width);
            }
            return hr;
        }
        using CreatePlacedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
            D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreatePlacedResource(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* desc,
            D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreatePlacedFn>(g_orig[kCreatePlacedResource])(d, heap, off, desc, st, cv, riid, out);
            if (SUCCEEDED(hr) && out && *out && heap && desc && ResourceIid(riid)) {
                const D3D12_HEAP_DESC hd = heap->GetDesc();
                if (CpuWritable(hd.Properties)) RegisterUpload(static_cast<ID3D12Resource*>(*out), desc->Dimension, desc->Width);
            }
            return hr;
        }
        using CreateCommitted1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
            const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreateCommittedResource1(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
            const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps,
            REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreateCommitted1Fn>(g_orig[kCreateCommittedResource1])(d, hp, hf, desc, st, cv, ps, riid, out);
            if (SUCCEEDED(hr) && out && *out && hp && desc && CpuWritable(*hp) && ResourceIid(riid)) {
                RegisterUpload(static_cast<ID3D12Resource*>(*out), desc->Dimension, desc->Width);
            }
            return hr;
        }
        // 0.4.2: the newer creation calls (Device8 / Device10), registered the same way.
        using CreateCommitted2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
            const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreateCommittedResource2(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
            const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps,
            REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreateCommitted2Fn>(g_orig[kCreateCommittedResource2])(d, hp, hf, desc, st, cv, ps, riid, out);
            if (SUCCEEDED(hr) && out && *out && hp && desc && CpuWritable(*hp) && ResourceIid(riid)) {
                RegisterUpload(static_cast<ID3D12Resource*>(*out), desc->Dimension, desc->Width);
            }
            return hr;
        }
        using CreatePlaced1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
            D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreatePlacedResource1(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC1* desc,
            D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreatePlaced1Fn>(g_orig[kCreatePlacedResource1])(d, heap, off, desc, st, cv, riid, out);
            if (SUCCEEDED(hr) && out && *out && heap && desc && ResourceIid(riid) && CpuWritable(heap->GetDesc().Properties)) {
                RegisterUpload(static_cast<ID3D12Resource*>(*out), desc->Dimension, desc->Width);
            }
            return hr;
        }
        using CreateCommitted3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
            const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32,
            const DXGI_FORMAT*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreateCommittedResource3(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
            const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps,
            UINT32 nf, const DXGI_FORMAT* fmts, REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreateCommitted3Fn>(g_orig[kCreateCommittedResource3])(d, hp, hf, desc, layout, cv, ps, nf, fmts, riid, out);
            if (SUCCEEDED(hr) && out && *out && hp && desc && CpuWritable(*hp) && ResourceIid(riid)) {
                RegisterUpload(static_cast<ID3D12Resource*>(*out), desc->Dimension, desc->Width);
            }
            return hr;
        }
        using CreatePlaced2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
            D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);
        HRESULT STDMETHODCALLTYPE H_CreatePlacedResource2(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC1* desc,
            D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* cv, UINT32 nf, const DXGI_FORMAT* fmts, REFIID riid, void** out)
        {
            const HRESULT hr = reinterpret_cast<CreatePlaced2Fn>(g_orig[kCreatePlacedResource2])(d, heap, off, desc, layout, cv, nf, fmts, riid, out);
            if (SUCCEEDED(hr) && out && *out && heap && desc && ResourceIid(riid) && CpuWritable(heap->GetDesc().Properties)) {
                RegisterUpload(static_cast<ID3D12Resource*>(*out), desc->Dimension, desc->Width);
            }
            return hr;
        }

        using CreateRtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
        void STDMETHODCALLTYPE H_CreateRenderTargetView(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
            D3D12_CPU_DESCRIPTOR_HANDLE dst)
        {
            reinterpret_cast<CreateRtvFn>(g_orig[kCreateRenderTargetView])(d, r, desc, dst);
            std::lock_guard lock(g_rtvLock);
            if (r) {
                g_rtvs[dst.ptr] = { r, desc ? desc->Format : r->GetDesc().Format };
            } else {
                g_rtvs.erase(dst.ptr);
            }
        }

        void HookDevice(ID3D12Device* device)
        {
            static std::once_flag once;
            std::call_once(once, [device] {
                g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                g_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                auto** vt = *reinterpret_cast<void***>(device);
                const std::pair<std::size_t, void*> hooks[] = {
                    { kCreateDescriptorHeap, reinterpret_cast<void*>(&H_CreateDescriptorHeap) },
                    { kCreateConstantBufferView, reinterpret_cast<void*>(&H_CreateConstantBufferView) },
                    { kCreateShaderResourceView, reinterpret_cast<void*>(&H_CreateShaderResourceView) },
                    { kCreateUnorderedAccessView, reinterpret_cast<void*>(&H_CreateUnorderedAccessView) },
                    { kCopyDescriptors, reinterpret_cast<void*>(&H_CopyDescriptors) },
                    { kCopyDescriptorsSimple, reinterpret_cast<void*>(&H_CopyDescriptorsSimple) },
                    { kCreateRenderTargetView, reinterpret_cast<void*>(&H_CreateRenderTargetView) },
                    { kCreateCommittedResource, reinterpret_cast<void*>(&H_CreateCommittedResource) },
                    { kCreatePlacedResource, reinterpret_cast<void*>(&H_CreatePlacedResource) },
                };
                bool ok = true;
                for (const auto& [index, fn] : hooks) {
                    g_orig[index] = vt[index];
                    ok &= PatchPointer(&vt[index], fn, nullptr);
                }
                // CreateCommittedResource1 only when the same object exposes ID3D12Device4 on this vtable.
                ID3D12Device4* d4 = nullptr;
                if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d4))) && d4) {
                    if (*reinterpret_cast<void***>(d4) == vt) {
                        g_orig[kCreateCommittedResource1] = vt[kCreateCommittedResource1];
                        PatchPointer(&vt[kCreateCommittedResource1], reinterpret_cast<void*>(&H_CreateCommittedResource1), nullptr);
                    }
                    d4->Release();
                }
                // 0.4.2: same for the Device8 / Device10 creation calls.
                int newer = 0;
                ID3D12Device8* d8 = nullptr;
                if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d8))) && d8) {
                    if (*reinterpret_cast<void***>(d8) == vt) {
                        g_orig[kCreateCommittedResource2] = vt[kCreateCommittedResource2];
                        PatchPointer(&vt[kCreateCommittedResource2], reinterpret_cast<void*>(&H_CreateCommittedResource2), nullptr);
                        g_orig[kCreatePlacedResource1] = vt[kCreatePlacedResource1];
                        PatchPointer(&vt[kCreatePlacedResource1], reinterpret_cast<void*>(&H_CreatePlacedResource1), nullptr);
                        newer |= 1;
                    }
                    d8->Release();
                }
                ID3D12Device10* d10 = nullptr;
                if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d10))) && d10) {
                    if (*reinterpret_cast<void***>(d10) == vt) {
                        g_orig[kCreateCommittedResource3] = vt[kCreateCommittedResource3];
                        PatchPointer(&vt[kCreateCommittedResource3], reinterpret_cast<void*>(&H_CreateCommittedResource3), nullptr);
                        g_orig[kCreatePlacedResource2] = vt[kCreatePlacedResource2];
                        PatchPointer(&vt[kCreatePlacedResource2], reinterpret_cast<void*>(&H_CreatePlacedResource2), nullptr);
                        newer |= 2;
                    }
                    d10->Release();
                }
                REX::INFO("Starfield RT: CPU-writable buffer registry hooks: Device8 {}, Device10 {}", (newer & 1) ? "yes" : "no",
                    (newer & 2) ? "yes" : "no");
                g_ready = ok;
                REX::INFO("Starfield RT: device {:p} created; descriptor hooks {} (increment {})", static_cast<void*>(device),
                    ok ? "installed" : "FAILED", g_increment);
            });
        }

        bool                       g_dredWanted = false;
        bool                       g_dredOn = false;
        bool                       g_dredMarks = false;  // 0.5.123
        std::atomic<bool>          g_removedLogged{ false };

        void TurnOnDred()
        {
            ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))) && settings) {
                settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                settings->Release();
                g_dredOn = true;
                // 0.5.123: DRED 1.2 breadcrumb contexts (the text of SetMarker calls), for DredMark.
                ID3D12DeviceRemovedExtendedDataSettings1* settings1 = nullptr;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings1))) && settings1) {
                    settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    settings1->Release();
                    g_dredMarks = true;
                }
                REX::INFO("Starfield RT: DRED on (auto-breadcrumbs and page-fault reporting) for crash diagnostics");
            } else {
                REX::WARN("Starfield RT: DRED unavailable (D3D12GetDebugInterface failed)");
            }
        }

        using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        HRESULT WINAPI H_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out)
        {
            if (g_dredWanted && !g_dredOn) TurnOnDred();
            const HRESULT hr = reinterpret_cast<CreateDeviceFn>(g_origCreateDevice)(adapter, level, riid, out);
            if (SUCCEEDED(hr) && out && *out) {
                ID3D12Device* device = nullptr;
                if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&device))) && device) {
                    HookDevice(device);
                    device->Release();
                }
            }
            return hr;
        }

        const char* AllocTypeName(D3D12_DRED_ALLOCATION_TYPE t)
        {
            switch (t) {
            case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command list";
            case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor heap";
            case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
            case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
            case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline state";
            default: return "other";
            }
        }

        std::string Narrow(const wchar_t* w)
        {
            std::string s;
            for (; w && *w && s.size() < 80; ++w) s.push_back(*w < 128 ? static_cast<char>(*w) : '?');
            return s;
        }

        void LogAllocations(const char* what, const D3D12_DRED_ALLOCATION_NODE* node)
        {
            int n = 0;
            for (; node && n < 12; node = node->pNext, ++n) {
                REX::INFO("Starfield RT: DRED   {} allocation: {} '{}'", what, AllocTypeName(node->AllocationType),
                    node->ObjectNameA ? node->ObjectNameA : Narrow(node->ObjectNameW).c_str());
            }
            if (!n) REX::INFO("Starfield RT: DRED   no {} allocations at the faulting address", what);
        }
    }

    void EnableDred(bool on) { g_dredWanted = on; }
    void SetCubemapBlanking(ID3D12Resource* probeArray, ID3D12Resource* black, DXGI_FORMAT format, UINT slices)
    {
        g_blankFormat = format;
        g_blankSlices = slices;
        g_blankBlack = black;
        g_blankProbe = probeArray;
    }
    void EnableCubemapBlanking(bool on) { g_blankOn = on; }
    void SuppressCubemapBlankingOnThisThread(bool on) { t_noBlank = on; }
    std::uint64_t CubemapBlankCount() { return g_blankCount.load(); }
    bool DredMarksOn() { return g_dredMarks; }

    ID3D12Resource* FindUploadBuffer(D3D12_GPU_VIRTUAL_ADDRESS va, std::uint64_t& offset)
    {
        std::lock_guard lock(g_uploadLock);
        auto it = g_uploads.upper_bound(va);
        if (it == g_uploads.begin()) return nullptr;
        --it;
        if (va >= it->first + it->second.size) return nullptr;
        offset = va - it->first;
        return it->second.res;
    }

    std::uint64_t UploadBufferCount() { return g_uploadCount.load(); }

    bool LookupRtv(SIZE_T cpu, ID3D12Resource*& resource, DXGI_FORMAT& format)
    {
        std::lock_guard lock(g_rtvLock);
        auto it = g_rtvs.find(cpu);
        if (it == g_rtvs.end()) return false;
        resource = it->second.res;
        format = it->second.format;
        return true;
    }

    UINT RtvIncrement() { return g_rtvIncrement; }

    void SetCbvCallback(CbvCallback cb) { g_cbvCallback = cb; }

    namespace
    {
        std::mutex                         g_vramLock;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> g_adapter;
        bool                               g_adapterTried = false;
        std::uint64_t                      g_vramPeak = 0;
    }

    void LogVram(ID3D12Device* device, const char* when)
    {
        if (!device) return;
        std::lock_guard lock(g_vramLock);
        if (!g_adapterTried) {
            g_adapterTried = true;
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&g_adapter));
            }
        }
        if (!g_adapter) return;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return;
        g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
        g_vramPeak = (std::max)(g_vramPeak, local.CurrentUsage);
        constexpr double mb = 1.0 / 1048576.0;
        REX::INFO("Starfield RT: VRAM ({}): {:.0f} of {:.0f} MB budget used ({:.0f}%), peak {:.0f} MB; shared {:.0f} MB", when,
            local.CurrentUsage * mb, local.Budget * mb, local.Budget ? 100.0 * local.CurrentUsage / local.Budget : 0.0, g_vramPeak * mb,
            shared.CurrentUsage * mb);
    }

    float VramUsedFraction(ID3D12Device* device)  // 0.5.100
    {
        if (!device) return 0.0f;
        std::lock_guard lock(g_vramLock);
        if (!g_adapterTried) {
            g_adapterTried = true;
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&g_adapter));
            }
        }
        if (!g_adapter) return 0.0f;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) || !local.Budget) return 0.0f;
        return static_cast<float>(static_cast<double>(local.CurrentUsage) / static_cast<double>(local.Budget));
    }

    bool CheckDeviceRemoved(ID3D12Device* device)
    {
        if (!device) return false;
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (reason == S_OK) return false;
        if (g_removedLogged.exchange(true)) return true;
        LogVram(device, "at device removal");
        REX::WARN("Starfield RT: GPU DEVICE REMOVED, reason {:#x} ({})", static_cast<unsigned>(reason),
            reason == DXGI_ERROR_DEVICE_HUNG ? "hung" : reason == DXGI_ERROR_DEVICE_RESET ? "reset" :
            reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "driver internal error" : reason == DXGI_ERROR_INVALID_CALL ? "invalid call" :
            reason == DXGI_ERROR_DEVICE_REMOVED ? "removed (page fault or similar)" : "other");
        ID3D12DeviceRemovedExtendedData* dred = nullptr;
        if (!g_dredOn || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
            REX::WARN("Starfield RT: DRED data unavailable ({})", g_dredOn ? "QueryInterface failed" : "DRED not enabled");
            return true;
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
            REX::INFO("Starfield RT: DRED page fault at GPU VA {:#x}", fault.PageFaultVA);
            LogAllocations("existing", fault.pHeadExistingAllocationNode);
            LogAllocations("recently freed", fault.pHeadRecentFreedAllocationNode);
        }
        // 0.5.123: DRED 1.2 output with breadcrumb contexts (our DredMark labels). For each unfinished list: the ops
        // around where the GPU stopped, the last plugin label at or before that op (the pass it was in, if ours), and
        // every plugin label in the list.
        ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs1{};
        const bool have1 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred1))) && dred1 && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&crumbs1));
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
        const D3D12_AUTO_BREADCRUMB_NODE* head0 = nullptr;
        if (!have1 && SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) head0 = crumbs.pHeadAutoBreadcrumbNode;
        const auto opName = [](D3D12_AUTO_BREADCRUMB_OP op) -> const char* {
            switch (op) {
            case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "marker";
            case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "begin event";
            case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "end event";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "draw";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "draw indexed";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "execute indirect";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "dispatch";
            case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "copy buffer";
            case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "copy texture";
            case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "copy resource";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "clear RTV";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "clear UAV";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "clear DSV";
            case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "barrier";
            case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "write immediate";
            case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "AS build";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "dispatch rays";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return "meta command";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "dispatch mesh";
            case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST: return "begin list";
            default: return nullptr;
            }
        };
        const auto logList = [&](const std::string& name, const void* list, UINT done, UINT count, const D3D12_AUTO_BREADCRUMB_OP* hist,
                                 const D3D12_DRED_BREADCRUMB_CONTEXT* ctx, UINT nctx) {
            REX::INFO("Starfield RT: DRED   unfinished command list '{}' ({:p}): {} of {} ops done", name, list, done, count);
            for (UINT i = done > 3 ? done - 3 : 0; i < count && i < done + 3; ++i) {
                const char* on = opName(hist[i]);
                REX::INFO("Starfield RT: DRED     op {}{}: {}", i, i == done ? " (<- GPU stopped here)" : "",
                    on ? std::string(on) : std::to_string(static_cast<int>(hist[i])));
            }
            if (!ctx || !nctx) return;
            const D3D12_DRED_BREADCRUMB_CONTEXT* last = nullptr;
            std::string all;
            for (UINT k = 0; k < nctx; ++k) {
                if (ctx[k].BreadcrumbIndex <= done && (!last || ctx[k].BreadcrumbIndex >= last->BreadcrumbIndex)) last = &ctx[k];
                if (all.size() < 1500) all += std::format(" [{}] {}", ctx[k].BreadcrumbIndex, Narrow(ctx[k].pContextString));
            }
            if (last) REX::INFO("Starfield RT: DRED     last label before the stop: op {} '{}' ({} ops later)", last->BreadcrumbIndex,
                Narrow(last->pContextString), done - last->BreadcrumbIndex);
            else REX::INFO("Starfield RT: DRED     no label before the stop");
            REX::INFO("Starfield RT: DRED     labels in this list:{}", all);
        };
        int lists = 0;
        if (have1) {
            for (const auto* node = crumbs1.pHeadAutoBreadcrumbNode; node && lists < 64; node = node->pNext, ++lists) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                if (done >= node->BreadcrumbCount) continue;  // this list finished
                const std::string name = node->pCommandListDebugNameA ? node->pCommandListDebugNameA : Narrow(node->pCommandListDebugNameW);
                logList(name, node->pCommandList, done, node->BreadcrumbCount, node->pCommandHistory, node->pBreadcrumbContexts,
                    node->BreadcrumbContextsCount);
            }
        } else {
            for (const auto* node = head0; node && lists < 64; node = node->pNext, ++lists) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                if (done >= node->BreadcrumbCount) continue;
                const std::string name = node->pCommandListDebugNameA ? node->pCommandListDebugNameA : Narrow(node->pCommandListDebugNameW);
                logList(name, node->pCommandList, done, node->BreadcrumbCount, node->pCommandHistory, nullptr, 0);
            }
        }
        if (dred1) dred1->Release();
        dred->Release();
        return true;
    }

    namespace
    {
    }

    bool InstallDeviceCreationHook()
    {
        auto** slot = reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + kIatD3D12CreateDeviceRva);
        HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
        void* expected = sl ? reinterpret_cast<void*>(GetProcAddress(sl, "D3D12CreateDevice")) : nullptr;
        if (!expected || *slot != expected) {
            REX::WARN("Starfield RT: D3D12CreateDevice import mismatch (slot {:p}, sl.interposer {:p}); device hooks skipped", *slot, expected);
            return false;
        }
        if (!PatchPointer(slot, reinterpret_cast<void*>(&H_D3D12CreateDevice), &g_origCreateDevice)) {
            REX::WARN("Starfield RT: could not patch D3D12CreateDevice import ({})", GetLastError());
            return false;
        }
        REX::INFO("Starfield RT: D3D12CreateDevice import hooked");
        return true;
    }

    bool DeviceHooksReady() { return g_ready.load(); }

    void LogHeaps(std::uint64_t probeGpu)
    {
        std::shared_lock lock(g_heapLock);
        for (const auto& h : g_heaps) {
            std::uint32_t views = 0;
            for (std::uint32_t i = 0; i < h->count; ++i) views += h->views[i].valid() || h->views[i].texture ? 1u : 0u;
            const bool has = h->gpuStart && probeGpu >= h->gpuStart && probeGpu < h->gpuStart + UINT64(h->count) * g_increment;
            REX::INFO("Starfield RT:   heap cpu {:#x} gpu {:#x} count {} views {}{}", h->cpuStart, h->gpuStart, h->count, views,
                has ? "  <- contains the handle" : "");
        }
    }

    std::uint32_t HeapIndexOf(D3D12_GPU_DESCRIPTOR_HANDLE handle)
    {
        std::shared_lock lock(g_heapLock);
        Heap* h = HeapForGpu(handle.ptr);
        return h ? static_cast<std::uint32_t>((handle.ptr - h->gpuStart) / g_increment) : UINT32_MAX;
    }

    bool GpuTableToCpu(D3D12_GPU_DESCRIPTOR_HANDLE tableBase, std::uint32_t offset, D3D12_CPU_DESCRIPTOR_HANDLE& out)
    {
        std::shared_lock lock(g_heapLock);
        Heap* h = HeapForGpu(tableBase.ptr);
        if (!h) return false;
        const UINT64 index = (tableBase.ptr - h->gpuStart) / g_increment + offset;
        if (index >= h->count) return false;
        out.ptr = h->cpuStart + SIZE_T(index) * g_increment;
        return true;
    }

    bool LookupHeapIndex(D3D12_GPU_DESCRIPTOR_HANDLE anyHandle, std::uint32_t index, BufferView& out)
    {
        std::shared_lock lock(g_heapLock);
        Heap* h = HeapForGpu(anyHandle.ptr);
        if (!h || index >= h->count || !h->views[index].valid()) {
            return false;
        }
        out = h->views[index];
        return true;
    }

    std::int64_t HeldBufferCount() { return g_ownedCount.load(std::memory_order_relaxed); }

    bool LookupHeapIndexRef(D3D12_GPU_DESCRIPTOR_HANDLE anyHandle, std::uint32_t index, BufferView& out, Microsoft::WRL::ComPtr<ID3D12Resource>& keep)
    {
        keep.Reset();
        std::shared_lock lock(g_heapLock);
        Heap* h = HeapForGpu(anyHandle.ptr);
        if (!h || !h->owned || index >= h->count || !h->views[index].valid() || !h->views[index].resource) {
            return false;
        }
        std::lock_guard own(g_ownLock);
        if (h->owned[index] != h->views[index].resource) return false;  // not held by us: don't touch it
        out = h->views[index];
        keep = h->owned[index];  // AddRef while the slot's own reference keeps it alive
        return true;
    }

    bool LookupGpuTable(D3D12_GPU_DESCRIPTOR_HANDLE tableBase, std::uint32_t offset, BufferView& out)
    {
        const std::uint32_t base = HeapIndexOf(tableBase);
        return base != UINT32_MAX && LookupHeapIndex(tableBase, base + offset, out);
    }

    bool LookupGpuTableAny(D3D12_GPU_DESCRIPTOR_HANDLE tableBase, std::uint32_t offset, BufferView& out)
    {
        const std::uint32_t base = HeapIndexOf(tableBase);
        if (base == UINT32_MAX) return false;
        std::shared_lock lock(g_heapLock);
        Heap* h = HeapForGpu(tableBase.ptr);
        const std::uint32_t i = base + offset;
        if (!h || i >= h->count || !(h->views[i].valid() || h->views[i].texture)) return false;
        out = h->views[i];
        return true;
    }

    std::uint32_t CountBufferViews(D3D12_GPU_DESCRIPTOR_HANDLE anyHandle, std::uint32_t first, std::uint32_t count)
    {
        std::shared_lock lock(g_heapLock);
        Heap* h = HeapForGpu(anyHandle.ptr);
        if (!h) {
            return 0;
        }
        std::uint32_t n = 0;
        for (std::uint32_t i = first; i < first + count && i < h->count; ++i) {
            n += h->views[i].resource != nullptr;  // buffer SRV/UAVs only
        }
        return n;
    }

    void LogDeviceHookStats()
    {
        std::shared_lock lock(g_heapLock);
        std::size_t shaderVisible = 0;
        for (auto& h : g_heaps) {
            shaderVisible += h->gpuStart != 0;
        }
        REX::INFO("Starfield RT: descriptor tracking: {} CBV/SRV/UAV heaps ({} shader-visible), {} buffer SRVs seen, {} copy calls",
            g_heaps.size(), shaderVisible, g_bufferViewsSeen.load(), g_copies.load());
    }
}
