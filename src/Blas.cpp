// BLAS builder (see Blas.h).
// Mesh record (52 B): +0 position handle, +28 bone-weight handle (0 = rigid), +32 index count,
// +36 first index (uint16 units into the #132 pool), +44 vertex count, +48 position scale.
// Handle: slot = v & 0x3FFF (heap index 32768 + slot), offset = (v >> 6) & 0xFFFF00.

#include <SFSE/SFSE.h>  // CommonLibSF must come before any Windows headers

#include "Blas.h"
#include "DeviceHooks.h"
#include "generated/BuildInstancesCS.h"  // compiled from shaders/BuildInstances.hlsl
#include "generated/SkinVerticesCS.h"     // 0.2.32: compiled from shaders/SkinVertices.hlsl
#include "generated/CopyGeometryCS.h"    // 0.1.34: compiled from shaders/CopyGeometry.hlsl
#include "GpuTimer.h"
#include "Reflections.h"  // 0.2.63: ForgetReflectionInputs
#include "RtShadows.h"    // 0.5.134: PhantomBits

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace rt
{
    namespace
    {
        constexpr std::uint32_t kTableMeshes = 5;      // t3 space2
        constexpr std::uint32_t kTablePool = 7;        // t5 space2
        constexpr std::uint32_t kBindlessFirst = 32768;
        constexpr std::uint32_t kMeshStride = 52;
        // frames before releasing replaced GPU buffers. 0.5.5: 30 -> 120. Both 0.5.3/0.5.4 load hangs were a page fault on a
        // just-freed "StarfieldRT buffer" during an acceleration-structure build, ~0.2 s after the TLAS buffers were swapped.
        constexpr std::uint64_t kGraveyardFrames = 120;
        constexpr std::uint32_t kPoolFirstSlot = 14;    // bindless slots 14.. are 16 MB windows of the pool
        constexpr std::uint64_t kPoolWindow = 16ull * 1024 * 1024;

        struct MeshKey
        {
            std::uint32_t posHandle = 0, firstIndex = 0, indexCount = 0, vertexCount = 0;
            bool operator==(const MeshKey&) const = default;
        };

        struct BlasEntry
        {
            MeshKey                key{};
            std::int32_t           page = -1;        // 0.2.7: suballocated from a BLAS page (-1 = none)
            std::uint64_t          offset = 0;
            D3D12_GPU_VIRTUAL_ADDRESS va = 0;
            std::uint64_t          bytes = 0;
            bool                   queued = false;
            bool                   built = false;
            bool                   skipped = false;  // 0.1.27: skipped for this key (pool-only); don't re-queue every scan
            MeshKey                pendingKey{};     // 0.1.34: a new/changed record must stay the same for kSettleFrames
            MeshKey                buildKey{};       // 0.2.9: the record a queued (re)build is for; the old BLAS stays live until then
            std::uint64_t          pendingSince = 0;
        };
        constexpr std::uint64_t kSettleFrames = 30;
        // 0.5.11: outside loads a changed mesh (the game swapping in a closer level of detail) settles for 10 frames, not
        // 30, so it rejoins RT shadows and reflections sooner (pop-in). Loads keep 30: they hung on half-loaded meshes.
        constexpr std::uint64_t kSettleFramesQuiet = 10;

        BlasSettings                       g_settings;
        std::mutex                         g_lock;
        std::vector<BlasEntry>             g_entries;  // by mesh index
        std::deque<std::uint32_t>          g_queue;
        ComPtr<ID3D12Resource>             g_scratch;
        std::uint64_t                      g_scratchSize = 0;
        std::vector<std::pair<std::uint64_t, ComPtr<ID3D12Resource>>> g_graveyard;
        std::uint64_t                      g_totalBytes = 0, g_built = 0, g_failed = 0, g_skipped = 0;
        std::uint64_t                      g_stale = 0, g_outOfBounds = 0;
        std::uint64_t                      g_retainedStreamed = 0;  // 0.1.31: streamed vertex buffers held for builds
        std::uint64_t                      g_replacements = 0;      // 0.2.9: rebuilds queued for meshes whose record changed
        std::uint64_t                      g_slotReuse = 0;         // 0.2.10: mesh slots reused for a different mesh (retired at once)
        std::uint64_t                      g_lastScan = 0, g_lastLog = 0;
        D3D12_GPU_VIRTUAL_ADDRESS          g_lastMeshVa = 0;
        std::uint64_t                      g_lastMeshSize = 0;
        bool                               g_budgetWarned = false;
        // 0.1.27: mesh statistics from the last scan (logged with the BLAS summary).
        struct ScanStats { std::uint32_t used = 0, inPool = 0, outOfPool = 0, skinned = 0, tooBig = 0, settling = 0; } g_scanStats;

        // 0.1.34: staging ring for sanitized geometry (copy pass, then BLAS builds read only our own data).
        constexpr std::uint32_t kStagingRing = 3;
        constexpr std::uint64_t kStagingBytes = 32ull * 1024 * 1024;
        struct Staging
        {
            ComPtr<ID3D12Resource>      buffers[kStagingRing];
            ComPtr<ID3D12RootSignature> rootSig;
            ComPtr<ID3D12PipelineState> pso;
            bool                        failed = false;
        } g_staging;

        ComPtr<ID3D12Resource> CreateGpuBuffer(ID3D12Device* device, std::uint64_t size, D3D12_RESOURCE_STATES state);

        // ---- 0.2.7: BLAS memory pool. One committed resource per BLAS cost a driver allocation per build, which
        // made streaming CPU-heavy (6-7 ms/frame) and limited builds per frame. BLASes now live in 64 MB pages,
        // carved with a best-fit free list (256-byte aligned). Freed ranges wait kGraveyardFrames before reuse.
        constexpr std::uint64_t kPageBytes = 64ull * 1024 * 1024;
        struct BlasPage
        {
            ComPtr<ID3D12Resource>            res;
            D3D12_GPU_VIRTUAL_ADDRESS         va = 0;
            std::uint64_t                     size = 0;
            std::map<std::uint64_t, std::uint64_t> freeRanges;  // offset -> size
        };
        std::vector<BlasPage> g_pages;
        struct PendingFree { std::uint64_t frame; std::int32_t page; std::uint64_t offset, size; };
        std::vector<PendingFree> g_pendingFrees;
        std::uint64_t g_pageBytes = 0;

        void FreeRange(std::int32_t page, std::uint64_t offset, std::uint64_t size)
        {
            auto& f = g_pages[page].freeRanges;
            auto it = f.emplace(offset, size).first;
            if (it != f.begin()) {  // merge with the previous range
                auto prev = std::prev(it);
                if (prev->first + prev->second == it->first) {
                    prev->second += it->second;
                    f.erase(it);
                    it = prev;
                }
            }
            auto next = std::next(it);
            if (next != f.end() && it->first + it->second == next->first) {
                it->second += next->second;
                f.erase(next);
            }
        }

        bool AllocBlas(ID3D12Device* device, std::uint64_t size, std::int32_t& page, std::uint64_t& offset)
        {
            size = (size + 255) & ~255ull;
            std::int32_t bestPage = -1;
            std::uint64_t bestOff = 0, bestSize = ~0ull;
            for (std::int32_t i = 0; i < static_cast<std::int32_t>(g_pages.size()); ++i) {
                for (const auto& [off, sz] : g_pages[i].freeRanges) {
                    if (sz >= size && sz < bestSize) {
                        bestPage = i; bestOff = off; bestSize = sz;
                        if (sz == size) break;
                    }
                }
            }
            if (bestPage < 0) {
                BlasPage np;
                np.size = (std::max)(kPageBytes, size);
                np.res = CreateGpuBuffer(device, np.size, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                if (!np.res) return false;
                np.va = np.res->GetGPUVirtualAddress();
                np.freeRanges.emplace(0, np.size);
                g_pageBytes += np.size;
                g_pages.push_back(std::move(np));
                bestPage = static_cast<std::int32_t>(g_pages.size()) - 1;
                bestOff = 0;
                bestSize = g_pages.back().size;
            }
            auto& f = g_pages[bestPage].freeRanges;
            f.erase(bestOff);
            if (bestSize > size) f.emplace(bestOff + size, bestSize - size);
            page = bestPage;
            offset = bestOff;
            return true;
        }

        void ReleasePendingFrees(std::uint64_t frame)
        {
            std::erase_if(g_pendingFrees, [frame](const PendingFree& p) {
                if (frame - p.frame <= kGraveyardFrames) return false;
                FreeRange(p.page, p.offset, p.size);
                return true;
            });
        }

        ComPtr<ID3D12Resource> CreateGpuBuffer(ID3D12Device* device, std::uint64_t size, D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = (size + 255) & ~std::uint64_t(255);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> r;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)))) {
                return nullptr;
            }
            r->SetName(state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE ? L"StarfieldRT acceleration structure" : L"StarfieldRT buffer");
            return r;
        }

        void Retire(BlasEntry& e, std::uint64_t frame)
        {
            if (e.page >= 0) {
                g_pendingFrees.push_back({ frame, e.page, e.offset, (e.bytes + 255) & ~255ull });
                g_totalBytes -= e.bytes;
            }
            e.page = -1;
            e.va = 0;
            e.bytes = 0;
            e.built = false;
        }

        // Copy the mesh records out of their UPLOAD heap and queue new/changed rigid meshes.
        void ScanMeshes(const BufferView& meshes, D3D12_GPU_DESCRIPTOR_HANDLE table, std::uint64_t frame)
        {
            void* base = nullptr;
            const std::uint64_t offset = meshes.gpuva - meshes.resource->GetGPUVirtualAddress();
            const D3D12_RANGE readRange{ static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + meshes.sizeBytes) };
            if (FAILED(meshes.resource->Map(0, &readRange, &base)) || !base) {
                return;
            }
            const std::size_t n = static_cast<std::size_t>(meshes.sizeBytes / kMeshStride);
            std::vector<std::uint8_t> recs(static_cast<const std::uint8_t*>(base) + offset,
                static_cast<const std::uint8_t*>(base) + offset + n * kMeshStride);
            const D3D12_RANGE noWrite{ 0, 0 };
            meshes.resource->Unmap(0, &noWrite);

            if (g_entries.size() < n) {
                g_entries.resize(n);
            }
            ScanStats stats{};
            for (std::size_t i = 0; i < n; ++i) {
                const auto* r = recs.data() + i * kMeshStride;
                std::uint32_t pos, bones, icount, first, vcount;
                std::memcpy(&pos, r + 0, 4);
                std::memcpy(&bones, r + 28, 4);
                std::memcpy(&icount, r + 32, 4);
                std::memcpy(&first, r + 36, 4);
                std::memcpy(&vcount, r + 44, 4);
                auto& e = g_entries[i];
                if (icount && vcount) {
                    ++stats.used;
                    if (bones != 0) ++stats.skinned;
                    else if (vcount > 65536) ++stats.tooBig;
                    else if ((pos & 0x3FFF) >= kPoolFirstSlot && (pos & 0x3FFF) < kPoolFirstSlot + 255) ++stats.inPool;
                    else ++stats.outOfPool;
                }
                const bool usable = icount && vcount && bones == 0 && icount % 3 == 0 && vcount <= 65536;
                if (!usable) {
                    if (e.built || e.queued) {
                        Retire(e, frame);
                        e.key = {};
                        e.buildKey = {};
                        e.queued = false;
                    }
                    continue;
                }
                const MeshKey key{ pos, first, icount, vcount };
                if (e.key == key && (e.built || e.queued || e.skipped)) {
                    continue;
                }
                e.skipped = false;
                // 0.1.34: a record that changed is rebuilt only after it has stayed the same for kSettleFrames, so
                // the game has finished uploading its geometry (save loads hung the GPU on half-loaded meshes).
                // 0.2.9: the old BLAS stays in the TLAS until the replacement is built (no shadow pop-out while the
                // game streams level-of-detail data); BLASes are built from our own copies, so the old one stays valid.
                // 0.2.10: only when the index and vertex counts are unchanged (the same mesh moved in memory). The game
                // also reuses mesh slots for different meshes; keeping the old BLAS then left a wrong, often huge,
                // invisible shape in the scene (a 10,000+ triangle BLAS on a 136-triangle mesh shadowed a whole wall).
                if ((e.built || e.queued) && (key.indexCount != e.key.indexCount || key.vertexCount != e.key.vertexCount)) {
                    Retire(e, frame);
                    e.key = {};
                    e.buildKey = {};
                    e.queued = false;  // a queued entry for the old mesh is dropped by BuildQueued's re-check
                    ++g_slotReuse;
                }
                if (!(e.pendingKey == key)) {
                    e.pendingKey = key;
                    e.pendingSince = frame;
                    ++stats.settling;
                    continue;
                }
                if (frame - e.pendingSince < (SceneSettling() ? kSettleFrames : kSettleFramesQuiet)) {
                    ++stats.settling;
                    continue;
                }
                BufferView vb{};
                if (!LookupHeapIndex(table, kBindlessFirst + (pos & 0x3FFF), vb) || !vb.resource) {
                    ++g_skipped;  // position buffer not tracked (yet)
                    continue;
                }
                e.buildKey = key;
                if (!e.queued) {
                    e.queued = true;
                    g_queue.push_back(static_cast<std::uint32_t>(i));
                    if (e.built) ++g_replacements;
                }
            }
            g_scanStats = stats;
        }

        bool EnsureStaging(ID3D12Device5* device)
        {
            if (g_staging.pso) return true;
            if (g_staging.failed) return false;
            g_staging.failed = true;
            for (auto& b : g_staging.buffers) {
                b = CreateGpuBuffer(device, kStagingBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (!b) return false;
            }
            D3D12_ROOT_PARAMETER p[4]{};
            p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants = { 0, 0, 6 };
            p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor = { 0, 0 };
            p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[2].Descriptor = { 1, 0 };
            p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[3].Descriptor = { 0, 0 };
            for (auto& q : p) q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            const D3D12_ROOT_SIGNATURE_DESC desc{ 4, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> blob, error;
            if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
                FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_staging.rootSig)))) {
                REX::WARN("Starfield RT: geometry copy root signature failed; BLAS builds disabled");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_staging.rootSig.Get();
            pd.CS = { g_CopyGeometryCS, sizeof(g_CopyGeometryCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_staging.pso)))) {
                REX::WARN("Starfield RT: geometry copy pipeline failed; BLAS builds disabled");
                return false;
            }
            g_staging.failed = false;
            REX::INFO("Starfield RT: geometry staging ready ({} x {} MB): BLAS builds read sanitized copies", kStagingRing,
                kStagingBytes / 1048576);
            return true;
        }

        struct BuildJob
        {
            std::uint32_t             mesh;
            D3D12_GPU_VIRTUAL_ADDRESS vertices;   // engine vertex data (256-aligned)
            UINT                      consts[6];  // CopyGeometry.hlsl Params
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build;
            D3D12_RAYTRACING_GEOMETRY_DESC geom;
            bool scratchBarrier = false;  // 0.2.7: wait for earlier builds before reusing the scratch buffer
        };

        void BuildQueued(ID3D12Device5* device5, ID3D12GraphicsCommandList4* list4, D3D12_GPU_DESCRIPTOR_HANDLE table,
            const BufferView& pool, const std::uint8_t* liveRecords, std::uint64_t liveBytes, std::uint64_t frame)
        {
            if (g_queue.empty() || !EnsureStaging(device5)) return;
            ID3D12Resource* staging = g_staging.buffers[frame % kStagingRing].Get();
            const D3D12_GPU_VIRTUAL_ADDRESS stagingVa = staging->GetGPUVirtualAddress();
            std::uint64_t stagingUsed = 0;
            std::uint64_t scratchUsed = 0;  // 0.2.7: per-build scratch slices
            std::vector<BuildJob> jobs;
            const std::uint64_t budget = std::uint64_t((std::max)(g_settings.budgetMB, 64)) * 1024 * 1024;
            int builds = 0;
            while (!g_queue.empty() && builds < g_settings.buildsPerFrame) {
                const std::uint32_t mi = g_queue.front();
                g_queue.pop_front();
                auto& e = g_entries[mi];
                e.queued = false;
                if (e.built && e.key == e.buildKey) continue;  // duplicate queue entry
                const MeshKey k = e.buildKey;
                // 0.1.12: the queue can be seconds old. Re-check the live record; if the mesh was
                // streamed out or its slot reused, skip it (the next scan re-queues it if needed).
                if (!liveRecords || std::uint64_t(mi + 1) * kMeshStride > liveBytes) {
                    e.buildKey = {};
                    ++g_stale;
                    continue;
                }
                {
                    MeshKey now{};
                    std::uint32_t bones = 0;
                    const auto* r = liveRecords + std::size_t(mi) * kMeshStride;
                    std::memcpy(&now.posHandle, r + 0, 4);
                    std::memcpy(&bones, r + 28, 4);
                    std::memcpy(&now.indexCount, r + 32, 4);
                    std::memcpy(&now.firstIndex, r + 36, 4);
                    std::memcpy(&now.vertexCount, r + 44, 4);
                    if (!(now == k) || bones != 0) {
                        e.buildKey = {};  // changed again; the old BLAS (if any) stays until the next settled record
                        ++g_stale;
                        continue;
                    }
                }
                const std::uint32_t slot = k.posHandle & 0x3FFF;
                const bool inPool = slot >= kPoolFirstSlot && std::uint64_t(slot - kPoolFirstSlot) * kPoolWindow < pool.sizeBytes;
                if (g_settings.poolOnly && !inPool) {
                    ++g_skipped;  // positions in a separate (streamed, freeable) buffer
                    e.skipped = true;
                    continue;
                }
                BufferView vb{};
                ComPtr<ID3D12Resource> vbKeep;  // 0.5.14: a safe reference (see LookupHeapIndexRef)
                if (!LookupHeapIndexRef(table, kBindlessFirst + slot, vb, vbKeep) || !vb.resource) {
                    ++g_skipped;
                    continue;
                }
                // Bounds: R16G16B16A16 at stride 6 reads 8 bytes for the last vertex (2 past the data).
                const std::uint64_t vbOffset = (k.posHandle >> 6) & 0xFFFF00;
                const std::uint64_t vbEnd = vbOffset + ((std::uint64_t(k.vertexCount) * 6 + 2 + 3) & ~3ull);  // copy reads whole dwords
                const std::uint64_t ibEnd = (std::uint64_t(k.firstIndex) + k.indexCount) * 2;
                if (vbEnd > vb.sizeBytes || ibEnd > pool.sizeBytes) {
                    ++g_outOfBounds;
                    e.buildKey = {};
                    continue;
                }
                // 0.1.31: a streamed (out-of-pool) vertex buffer can be released by the game right after this check,
                // while our recorded build has not run on the GPU yet (the game only keeps buffers alive for its own
                // GPU work). Hold a reference for kGraveyardFrames so the build never reads freed memory.
                if (!inPool) {
                    g_graveyard.emplace_back(frame, vbKeep);  // 0.5.14: was ComPtr(vb.resource): an AddRef on a maybe-freed buffer
                    ++g_retainedStreamed;
                }
                // 0.1.34: room in this frame's staging buffer for the sanitized copy.
                const std::uint64_t vBytes = (std::uint64_t(k.vertexCount) * 6 + 2 + 3) & ~3ull;
                const std::uint64_t iBytes = ((std::uint64_t(k.indexCount) + 1) / 2) * 4;
                const std::uint64_t vDst = stagingUsed;
                const std::uint64_t iDst = (vDst + vBytes + 255) & ~255ull;
                const std::uint64_t end = (iDst + iBytes + 255) & ~255ull;
                if (end > kStagingBytes) {
                    g_queue.push_front(mi);  // next frame
                    e.queued = true;
                    break;
                }
                D3D12_RAYTRACING_GEOMETRY_DESC geom{};
                geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;  // alpha-tested instances are forced non-opaque in the TLAS
                geom.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
                geom.Triangles.VertexFormat = DXGI_FORMAT_R16G16B16A16_SNORM;  // int16 xyz, A16 ignored (stride 6)
                geom.Triangles.IndexCount = k.indexCount;
                geom.Triangles.VertexCount = k.vertexCount;
                geom.Triangles.IndexBuffer = stagingVa + iDst;
                geom.Triangles.VertexBuffer.StartAddress = stagingVa + vDst;
                geom.Triangles.VertexBuffer.StrideInBytes = 6;

                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
                inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                inputs.NumDescs = 1;
                inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                inputs.pGeometryDescs = &geom;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
                device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
                if (!info.ResultDataMaxSizeInBytes) {
                    ++g_failed;
                    continue;
                }
                if (g_totalBytes + info.ResultDataMaxSizeInBytes > budget) {
                    if (!g_budgetWarned) {
                        REX::WARN("Starfield RT: BLAS memory budget ({} MB) reached; raise [RT] iBlasBudgetMB", g_settings.budgetMB);
                        g_budgetWarned = true;
                    }
                    e.buildKey = {};  // retry on a later scan if memory frees up
                    e.pendingKey = {};
                    continue;
                }
                // 0.2.7: each build in the batch gets its own scratch slice, so the GPU can overlap them; when
                // the scratch buffer is full, the next build waits for the earlier ones (UAV barrier) and reuses it.
                const std::uint64_t scratchNeed = (info.ScratchDataSizeInBytes + 255) & ~255ull;
                bool scratchBarrier = false;
                if (scratchNeed > g_scratchSize) {
                    if (g_scratch) {
                        g_graveyard.emplace_back(frame, std::move(g_scratch));
                    }
                    g_scratchSize = (std::max<std::uint64_t>)(scratchNeed, 32ull * 1024 * 1024);
                    g_scratch = CreateGpuBuffer(device5, g_scratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    scratchUsed = 0;
                    if (!g_scratch) {
                        g_scratchSize = 0;
                        ++g_failed;
                        continue;
                    }
                }
                if (scratchUsed + scratchNeed > g_scratchSize) {
                    scratchBarrier = true;
                    scratchUsed = 0;
                }
                std::int32_t newPage = -1;
                std::uint64_t newOffset = 0;
                if (!AllocBlas(device5, info.ResultDataMaxSizeInBytes, newPage, newOffset)) {
                    ++g_failed;
                    continue;
                }
                Retire(e, frame);  // 0.2.9: the old BLAS (if any) is swapped out in the same frame its replacement is built
                e.page = newPage;
                e.offset = newOffset;
                e.va = g_pages[e.page].va + e.offset;
                e.key = k;
                stagingUsed = end;
                BuildJob job{};
                job.mesh = mi;
                job.vertices = vb.gpuva + vbOffset;
                job.consts[0] = static_cast<UINT>(vBytes / 4);
                job.consts[1] = k.indexCount;
                job.consts[2] = k.vertexCount;
                job.consts[3] = k.firstIndex * 2;
                job.consts[4] = static_cast<UINT>(vDst);
                job.consts[5] = static_cast<UINT>(iDst);
                job.geom = geom;
                job.build.Inputs = inputs;
                job.build.DestAccelerationStructureData = e.va;
                job.build.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress() + scratchUsed;
                job.scratchBarrier = scratchBarrier;
                scratchUsed += scratchNeed;
                jobs.push_back(job);
                e.bytes = info.ResultDataMaxSizeInBytes;
                e.built = true;
                g_totalBytes += e.bytes;
                ++g_built;
                ++builds;
            }
            if (jobs.empty()) return;

            // Copy pass: sanitized vertices and clamped indices into the staging buffer.
            list4->SetComputeRootSignature(g_staging.rootSig.Get());
            list4->SetPipelineState(g_staging.pso.Get());
            list4->SetComputeRootShaderResourceView(2, pool.gpuva);
            list4->SetComputeRootUnorderedAccessView(3, stagingVa);
            for (const auto& j : jobs) {
                list4->SetComputeRoot32BitConstants(0, 6, j.consts, 0);
                list4->SetComputeRootShaderResourceView(1, j.vertices);
                const UINT threads = (std::max)(j.consts[0], (j.consts[1] + 1) / 2);
                rt::DredMark(list4, L"SFRT Blas staging #1");
                list4->Dispatch((threads + 63) / 64, 1, 1);
            }
            D3D12_RESOURCE_BARRIER toRead{};
            toRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRead.Transition = { staging, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
            list4->ResourceBarrier(1, &toRead);

            // Builds read only the staging copies.
            D3D12_RESOURCE_BARRIER uavAll{};
            uavAll.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // null resource: every UAV write
            for (auto& j : jobs) {
                if (j.scratchBarrier) list4->ResourceBarrier(1, &uavAll);
                j.build.Inputs.pGeometryDescs = &j.geom;
                rt::DredMark(list4, L"SFRT Blas AS build #2");
                list4->BuildRaytracingAccelerationStructure(&j.build, 0, nullptr);
            }
            list4->ResourceBarrier(1, &uavAll);  // results complete before the TLAS build and next frame's scratch use
            D3D12_RESOURCE_BARRIER back = toRead;
            back.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            back.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            list4->ResourceBarrier(1, &back);
        }
    }

    // ---------------------------------------------------------------- TLAS
    namespace
    {
        constexpr std::uint32_t kTableInstances = 2;  // t0 space2, 96 B
        constexpr std::uint32_t kTableLods = 4;       // t2 space2, 20 B
        constexpr std::uint32_t kTableMaterials = 11; // t9 space2, CommonMaterialEntry 288 B (0.1.23)
        constexpr std::uint32_t kTableVegMaterials = 13; // t11 space2, vegetation material entry 368 B (0.1.23)
        constexpr std::uint32_t kRing = 3;            // BLAS-address upload buffers in flight

        struct Tlas
        {
            ComPtr<ID3D12RootSignature> rootSig;
            ComPtr<ID3D12PipelineState> pso;
            bool                        pipelineFailed = false;
            ComPtr<ID3D12Resource>      addrRing[kRing];
            std::uint64_t*              addrMapped[kRing]{};
            std::uint32_t               addrCapacity = 0;  // meshes per ring buffer
            ComPtr<ID3D12Resource>      descs;             // instance descriptors (UAV state between frames)
            ComPtr<ID3D12Resource>      result;
            ComPtr<ID3D12Resource>      scratch;
            std::uint32_t               capacity = 0;      // instances the buffers were sized for
            std::uint64_t               resultBytes = 0, scratchBytes = 0;
            std::uint64_t               builds = 0;
            std::uint32_t               lastInstances = 0;
            // 0.2.2: compaction. The active count is read back kRing frames later and sizes NumDescs.
            ComPtr<ID3D12Resource>      counter;                 // 4-byte UAV
            ComPtr<ID3D12Resource>      counterReadback[kRing];
            const std::uint32_t*        counterMapped[kRing]{};
            std::uint32_t               activeCount = 0;         // last read-back active instance count
            std::uint32_t               limit = 0;               // NumDescs this frame
            std::uint64_t               countFrames = 0;         // frames with a counter copy in flight
            std::uint32_t               glassCount = 0;          // 0.2.44: active instances tagged as glass
            std::uint32_t               playerCount = 0;         // 0.2.67: skinned instances tagged as the first-person player
            std::uint32_t               sizeCulled = 0;          // 0.5.100: left out as too small on screen
            std::uint32_t               breakdown[11]{};         // 0.5.101: distance x4, radius x4, alpha, effect, characters
            std::uint32_t               dupMesh = 0, dupPlace = 0;  // 0.5.102: same mesh at the same place; any mesh at a taken place
        } g_tlas;
        std::atomic<D3D12_GPU_VIRTUAL_ADDRESS> g_tlasAddress{ 0 };
        std::atomic<bool>          g_sceneLoading{ false };
        std::atomic<std::uint64_t> g_quietUntil{ 0 };  // 0.3.7: frame until which effects other than shadows wait
        std::atomic<std::uint64_t> g_lastFrame{ 0 };
        // 0.5.103: frames per second (smoothed). The post-load waits were counted in frames, so at 35 fps rendered they
        // lasted 2.6 s (calm) + 5 s (quiet). Now they are the shorter of the old frame count and a fixed time.
        float g_fps = 60.0f;
        std::atomic<std::uint32_t> g_tableDumpReq{ 0 };  // 0.5.111: 0x80000000 | family << 16 | material
        float g_settleSeconds = 2.0f;  // 0.5.106: ini fRtSettleSeconds
        float g_fadeSeconds = 1.5f;    // 0.5.106: ini fRtFadeSeconds (0.5.104 had 0.75 s)
        std::atomic<long long> g_settleEndNs{ 0 };  // 0.5.104: steady_clock time when settling last ended (0 = still settling)
        std::uint64_t WaitFrames(std::uint64_t oldFrames, float seconds, std::uint64_t floorFrames)
        {
            const auto timed = static_cast<std::uint64_t>(g_fps * seconds);
            return (std::min)(oldFrames, (std::max)(floorFrames, timed));
        }

        // ---------------------------------------------------------------- 0.2.32: skinned characters
        // Skinned instances (NPCs, the player) near the camera get a slot: every frame a compute pass writes the
        // instance's current object-space positions (shaders/SkinVertices.hlsl) and a fast BLAS is built from them.
        // BuildInstances lists the candidates (read back kRing frames later) and uses a slot's BLAS for its instance.
        // 0.5.11: 64 -> 128 slots (Ultra crowds filled 64 fast: a character is several instances).
        constexpr std::uint32_t kSkinSlotsMax = 128;    // SkinSlots table layout: 128 ids, then 128 addresses at +512
        constexpr std::uint32_t kSkinAddrOffset = kSkinSlotsMax * 4;  // must match BuildInstances.hlsl
        constexpr std::uint32_t kSkinCandidates = 2048;  // must match BuildInstances.hlsl; 0.5.119: 256 -> 2048 (Neon filled 256, cut off in GPU order before the nearest-first sort)
        constexpr std::uint64_t kSkinCandBytes = 16 + kSkinCandidates * 16;
        constexpr std::uint64_t kSkinStaleFrames = 30;  // a slot whose instance stopped being a candidate is freed after this

        struct SkinSlot
        {
            std::uint32_t          instance = ~0u, mesh = 0, vcount = 0, icount = 0, firstIndex = 0;
            std::uint64_t          lastSeen = 0;
            ComPtr<ID3D12Resource> vb, ib, blas;  // 0.2.35: ib = own clamped 32-bit indices
            std::uint64_t          vbBytes = 0, ibBytes = 0, blasBytes = 0, scratchBytes = 0, scratchOffset = 0;
            bool                   builtThisFrame = false;
        };

        struct Skin
        {
            ComPtr<ID3D12RootSignature> rootSig;
            ComPtr<ID3D12PipelineState> pso;
            bool                        failed = false, ready = false;
            SkinSlot                    slots[kSkinSlotsMax];
            ComPtr<ID3D12Resource>      cands;                 // UAV
            ComPtr<ID3D12Resource>      slotState;             // 0.5.50: UAV, 32 B per slot (frozen-pose check)
            ComPtr<ID3D12Resource>      candsReadback[kRing];
            const std::uint32_t*        candsMapped[kRing]{};
            ComPtr<ID3D12Resource>      table[kRing];          // upload: slot ids + BLAS addresses
            std::uint32_t*              tableMapped[kRing]{};
            ComPtr<ID3D12Resource>      scratch;
            std::uint64_t               scratchBytes = 0;
            std::uint64_t               frames = 0;            // frames with a candidate copy in flight
            std::uint64_t               built = 0, lastLog = 0;
        } g_skin;
        std::atomic<bool> g_skinDumpRequested{ false };  // 0.5.52
        // 0.5.66: near-camera instance dump (Numpad .): BuildInstances writes it into the counter buffer after +64.
        constexpr std::uint64_t kCounterBytes = 64 + 48 * 64;            // near-camera dump (read back on request)
        constexpr std::uint64_t kCounterAllocBytes = 4096 + 2 * (1u << 19) * 4;  // 0.5.102: + duplicate counts at 4088 and two hash tables

        // 0.5.100: while local VRAM is nearly full (Jemison soak test: 97% of budget, 90-100 ms frames), shrink the RT scene
        // range and raise the size-on-screen cut, in steps once a second; grow back once there is room again.
        float g_vramScale = 1.0f;
        float VramPressureScale(ID3D12Device* device, std::uint64_t frame)
        {
            static std::uint64_t last = 0;
            if (frame - last < 60) return g_vramScale;
            last = frame;
            const float used = VramUsedFraction(device);
            const float before = g_vramScale;
            if (used > 0.90f)
                g_vramScale = (std::max)(0.4f, g_vramScale * 0.85f);
            else if (used < 0.82f && g_vramScale < 1.0f)
                g_vramScale = (std::min)(1.0f, g_vramScale * 1.1f);
            if (g_vramScale != before)
                REX::INFO("Starfield RT: VRAM {:.0f}% of budget: RT scene range x{:.2f} (size cut x{:.2f})", used * 100.0f, g_vramScale,
                    1.0f / g_vramScale);
            return g_vramScale;
        }
        std::atomic<bool> g_nearDumpRequested{ false };
        ComPtr<ID3D12Resource> g_nearDumpReadback;
        std::uint64_t g_nearDumpFrame = 0;  // frame the copy was recorded (0 = none pending)

        void LogNearDump(const std::uint32_t* d)
        {
            const std::uint32_t n = d[4];
            REX::INFO("Starfield RT: [near] {} instance record(s) within 1.5 m of the camera{}", n, n > 48 ? " (first 48 listed)" : "");
            for (std::uint32_t i = 0; i < (std::min)(n, 48u); ++i) {
                const std::uint32_t* e = d + 16 + i * 16;
                float f[9];
                std::memcpy(f, e + 4, sizeof(f));
                const std::uint32_t pk = e[3];
                REX::INFO("Starfield RT: [near]   record {} mesh {} bones {}: {} {}, hit group {}{}{}{}, half-size {:.2f} m, {:.2f} m from camera, "
                          "camera - origin ({:.2f}, {:.2f}, {:.2f}), rotation ({:.3f}, {:.3f}, {:.3f}, {:.3f})",
                    e[0], e[2], e[1], (pk >> 16) & 1u ? "IN TLAS" : "left out", (pk & 8u) ? "non-opaque" : "opaque", (pk >> 8) & 0xFFu,
                    (pk >> 8) & 0xFFu ? ((pk >> 8) & 0xFFu) == 1u ? " (PLAYER: skipped by reflections)" : " (character)" : "",
                    (pk >> 17) & 1u ? ", game-skinned this frame" : "", (pk >> 18) & 1u ? ", glass" : (pk >> 19) & 1u ? ", effect" : "",
                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
            }
        }
        // 0.5.50/0.5.53 SlotState layout: [0, 4096) 32 B per slot, 4096 last-move frame, [8192, +128 x 176) probe records
        constexpr std::uint64_t kSkinStateBytes = 8192 + kSkinSlotsMax * 176;
        struct SkinDumpRow { std::uint32_t slot, instance, mesh, verts, tris; float dist; };
        struct SkinDump
        {
            ComPtr<ID3D12Resource>   readback;
            std::uint64_t            pendingFrame = 0;
            std::vector<SkinDumpRow> rows;
        } g_skinDump;

        bool CreateSkinResources(ID3D12Device* device)
        {
            // Root signature 1.1 with direct heap indexing: positions and bone weights are bindless engine buffers.
            D3D12_ROOT_PARAMETER1 p[10]{};
            p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants = { 0, 0, 9 };  // 0.5.50: + slot, 0.5.51: + frame, 0.5.53: + dump
            for (UINT i = 0; i < 5; ++i) {  // t0..t3 records, t4 index pool (0.2.35)
                p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                p[1 + i].Descriptor = { i, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            }
            for (UINT i = 0; i < 3; ++i) {  // u0 positions, u1 indices (0.2.35), u2 slot state (0.5.50)
                p[6 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                p[6 + i].Descriptor = { i, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            }
            p[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // 0.5.50: b1 per-scene rows (camera position)
            p[9].Descriptor = { 1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            for (auto& q : p) q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
            desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            desc.Desc_1_1 = { 10, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED };
            ComPtr<ID3DBlob> blob, error;
            if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
                FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_skin.rootSig)))) {
                REX::WARN("Starfield RT: skinning root signature failed: {}", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_skin.rootSig.Get();
            pd.CS = { g_SkinVerticesCS, sizeof(g_SkinVerticesCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_skin.pso)))) {
                REX::WARN("Starfield RT: skinning pipeline failed");
                return false;
            }
            g_skin.cands = CreateGpuBuffer(device, kSkinCandBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!g_skin.cands) return false;
            g_skin.slotState = CreateGpuBuffer(device, kSkinStateBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);  // 0.5.50 (+ last-move frame at 4096, 0.5.51)
            if (!g_skin.slotState) return false;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            for (std::uint32_t i = 0; i < kRing; ++i) {
                D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
                rd.Width = kSkinCandBytes;
                void* m = nullptr;
                if (FAILED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                        IID_PPV_ARGS(&g_skin.candsReadback[i]))) || FAILED(g_skin.candsReadback[i]->Map(0, nullptr, &m)) || !m) {
                    return false;
                }
                g_skin.candsMapped[i] = static_cast<const std::uint32_t*>(m);
                D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
                rd.Width = kSkinAddrOffset + kSkinSlotsMax * 8;
                const D3D12_RANGE none{ 0, 0 };
                m = nullptr;
                if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                        IID_PPV_ARGS(&g_skin.table[i]))) || FAILED(g_skin.table[i]->Map(0, &none, &m)) || !m) {
                    return false;
                }
                g_skin.tableMapped[i] = static_cast<std::uint32_t*>(m);
                std::memset(m, 0xFF, kSkinAddrOffset);
                std::memset(static_cast<std::uint8_t*>(m) + kSkinAddrOffset, 0, kSkinSlotsMax * 8);
            }
            REX::INFO("Starfield RT: skinned-character support ready ({} slots within {} units)",
                (std::min)(static_cast<std::uint32_t>((std::max)(g_settings.skinSlots, 0)), kSkinSlotsMax), g_settings.skinRadius);
            return true;
        }

        // Before the instance pass: pick slots from the read-back candidates, skin them and build their BLASes, and
        // fill this frame's slot table. Records compute work (the caller resets the root signature afterwards).
        void UpdateSkinned(ID3D12Device5* device5, ID3D12GraphicsCommandList4* list4, D3D12_GPU_DESCRIPTOR_HANDLE table,
            const BufferView& meshes, D3D12_GPU_VIRTUAL_ADDRESS perScene, std::uint64_t frame)
        {
            const std::uint32_t ring = static_cast<std::uint32_t>(frame % kRing);
            std::uint32_t* tbl = g_skin.tableMapped[ring];
            for (auto& s : g_skin.slots) s.builtThisFrame = false;
            // 0.5.53: log the probe copy once the GPU is surely done with it
            if (g_skinDump.pendingFrame && frame >= g_skinDump.pendingFrame + kRing + 3 && g_skinDump.readback) {
                g_skinDump.pendingFrame = 0;
                void* m = nullptr;
                const D3D12_RANGE rr{ 0, static_cast<SIZE_T>(kSkinStateBytes) };
                if (SUCCEEDED(g_skinDump.readback->Map(0, &rr, &m)) && m) {
                    const auto* d = static_cast<const std::uint8_t*>(m);
                    std::uint32_t lastMove;
                    std::memcpy(&lastMove, d + 4096, 4);
                    REX::INFO("Starfield RT: [skin] probe: {} skinned copies in the RT scene (last pose change at frame {})",
                        g_skinDump.rows.size(), lastMove);
                    std::vector<SkinDumpRow> rows = g_skinDump.rows;
                    std::sort(rows.begin(), rows.end(), [](const SkinDumpRow& a, const SkinDumpRow& b) { return a.dist < b.dist; });
                    for (const auto& r : rows) {
                        std::uint32_t st[5];
                        std::memcpy(st, d + r.slot * 32, 20);
                        const auto* w = reinterpret_cast<const std::uint32_t*>(d + 8192 + r.slot * 176);
                        std::string ins, lod;
                        for (int i = 0; i < 24; ++i) ins += std::format(" {:08x}", w[i]);
                        for (int i = 24; i < 44; ++i) lod += std::format(" {:08x}", w[i]);
                        REX::INFO("Starfield RT: [skin]   instance {} mesh {} verts {} tris {} dist {:.2f} | frozen-while-others-moved {}{}",
                            r.instance, r.mesh, r.verts, r.tris, r.dist, st[3], st[4] == r.instance ? "" : " (state of another instance)");
                        REX::INFO("Starfield RT: [skin]     inst{}", ins);
                        REX::INFO("Starfield RT: [skin]     lod {}", lod);
                    }
                    const D3D12_RANGE none{ 0, 0 };
                    g_skinDump.readback->Unmap(0, &none);
                }
            }
            const std::uint32_t slotCount = (std::min)(static_cast<std::uint32_t>((std::max)(g_settings.skinSlots, 0)), kSkinSlotsMax);
            auto writeTable = [&] {
                for (std::uint32_t i = 0; i < kSkinSlotsMax; ++i) {
                    const auto& s = g_skin.slots[i];
                    const bool use = i < slotCount && s.builtThisFrame && s.blas;
                    tbl[i] = use ? s.instance : ~0u;
                    const std::uint64_t va = use ? s.blas->GetGPUVirtualAddress() : 0;
                    std::memcpy(reinterpret_cast<std::uint8_t*>(tbl) + kSkinAddrOffset + i * 8, &va, 8);
                }
            };
            if (!tbl) return;
            if (slotCount == 0 || g_settings.skinRadius <= 0.0f || SceneLoading()) {
                writeTable();
                return;
            }
            BufferView inst{}, lods{}, pool{}, tr{};
            if (!LookupGpuTable(table, kTableInstances, inst) || !LookupGpuTable(table, kTableLods, lods) ||
                !LookupGpuTable(table, 7, pool) || !pool.raw || !LookupGpuTable(table, 9, tr) || tr.stride != 48 || !meshes.resource) {
                writeTable();
                return;
            }
            const std::uint32_t meshCount = static_cast<std::uint32_t>(meshes.sizeBytes / kMeshStride);

            // Candidates recorded kRing frames ago (instance, mesh, distance), nearest first.
            struct Cand { std::uint32_t instance, mesh; float dist; };
            std::vector<Cand> cands;
            if (g_skin.frames >= kRing && g_skin.candsMapped[ring]) {
                const std::uint32_t* c = g_skin.candsMapped[ring];
                const std::uint32_t n = (std::min)(c[0], kSkinCandidates);
                cands.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    const std::uint32_t* e = c + 4 + i * 4;
                    float d;
                    std::memcpy(&d, &e[2], 4);
                    cands.push_back({ e[0], e[1], d });
                }
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
            }

            // Mesh records are in an upload heap: read the fields the BLAS needs on the CPU.
            void* mapped = nullptr;
            const std::uint64_t offset = meshes.gpuva - meshes.resource->GetGPUVirtualAddress();
            const D3D12_RANGE readRange{ static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + meshes.sizeBytes) };
            if (FAILED(meshes.resource->Map(0, &readRange, &mapped)) || !mapped) {
                writeTable();
                return;
            }
            const std::uint8_t* recs = static_cast<const std::uint8_t*>(mapped) + offset;
            auto geometry = [&](std::uint32_t mesh, SkinSlot& s) {
                if (mesh >= meshCount) return false;
                const std::uint8_t* r = recs + std::uint64_t(mesh) * kMeshStride;
                std::uint32_t icount, first, vcount;
                std::memcpy(&icount, r + 32, 4);
                std::memcpy(&first, r + 36, 4);
                std::memcpy(&vcount, r + 44, 4);
                if (!icount || icount % 3 || !vcount || vcount > 65536) return false;
                if ((std::uint64_t(first) + icount) * 2 > pool.sizeBytes) return false;
                s.mesh = mesh;
                s.icount = icount;
                s.firstIndex = first;
                s.vcount = vcount;
                return true;
            };

            // Keep slots whose instance is still a candidate; give free (or stale) slots to the nearest new ones.
            for (const auto& c : cands) {
                SkinSlot* hit = nullptr;
                for (std::uint32_t i = 0; i < slotCount; ++i) {
                    if (g_skin.slots[i].instance == c.instance) { hit = &g_skin.slots[i]; break; }
                }
                if (!hit) {
                    for (std::uint32_t i = 0; i < slotCount; ++i) {
                        auto& s = g_skin.slots[i];
                        if (s.instance == ~0u || s.lastSeen + kSkinStaleFrames < frame) { hit = &s; hit->instance = ~0u; break; }
                    }
                }
                if (!hit) break;  // all slots busy with nearer instances
                if (!geometry(c.mesh, *hit)) { hit->instance = ~0u; continue; }
                hit->instance = c.instance;
                hit->lastSeen = frame;
            }
            const D3D12_RANGE noWrite{ 0, 0 };
            meshes.resource->Unmap(0, &noWrite);

            // Size buffers for the active slots.
            std::uint64_t scratchTotal = 0;
            std::vector<SkinSlot*> active;
            for (std::uint32_t i = 0; i < slotCount; ++i) {
                auto& s = g_skin.slots[i];
                if (s.instance == ~0u) continue;
                if (s.lastSeen + kSkinStaleFrames < frame) { s.instance = ~0u; continue; }
                const std::uint64_t vbNeed = std::uint64_t(s.vcount) * 12;
                if (vbNeed > s.vbBytes) {
                    if (s.vb) g_graveyard.emplace_back(frame, std::move(s.vb));
                    s.vb = CreateGpuBuffer(device5, (vbNeed + 65535) & ~65535ull, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    s.vbBytes = s.vb ? ((vbNeed + 65535) & ~65535ull) : 0;
                }
                const std::uint64_t ibNeed = std::uint64_t(s.icount) * 4;
                if (ibNeed > s.ibBytes) {
                    if (s.ib) g_graveyard.emplace_back(frame, std::move(s.ib));
                    s.ib = CreateGpuBuffer(device5, (ibNeed + 65535) & ~65535ull, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    s.ibBytes = s.ib ? ((ibNeed + 65535) & ~65535ull) : 0;
                }
                D3D12_RAYTRACING_GEOMETRY_DESC geom{};
                geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geom.Triangles.VertexCount = s.vcount;
                geom.Triangles.VertexBuffer.StrideInBytes = 12;
                geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                geom.Triangles.IndexCount = s.icount;
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
                in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                in.NumDescs = 1;
                in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                in.pGeometryDescs = &geom;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
                device5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
                if (info.ResultDataMaxSizeInBytes > s.blasBytes) {
                    if (s.blas) g_graveyard.emplace_back(frame, std::move(s.blas));
                    const std::uint64_t sz = (info.ResultDataMaxSizeInBytes + 65535) & ~65535ull;
                    s.blas = CreateGpuBuffer(device5, sz, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                    s.blasBytes = s.blas ? sz : 0;
                }
                if (!s.vb || !s.ib || !s.blas) continue;
                s.scratchBytes = (info.ScratchDataSizeInBytes + 255) & ~255ull;
                s.scratchOffset = scratchTotal;
                scratchTotal += s.scratchBytes;
                active.push_back(&s);
            }
            if (scratchTotal > g_skin.scratchBytes) {
                if (g_skin.scratch) g_graveyard.emplace_back(frame, std::move(g_skin.scratch));
                const std::uint64_t sz = (std::max<std::uint64_t>)((scratchTotal + (1ull << 20) - 1) & ~((1ull << 20) - 1), 4ull << 20);
                g_skin.scratch = CreateGpuBuffer(device5, sz, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                g_skin.scratchBytes = g_skin.scratch ? sz : 0;
            }
            if (active.empty() || !g_skin.scratch) {
                writeTable();
                return;
            }

            // 0.5.53: probe: the skinning pass copies each active copy's instance record (96 B) and LOD record (80 B) to
            // SlotState (the records are in GPU-only memory); read back a few frames later and logged with the slot list.
            bool dumpNow = false;
            if (g_skinDump.pendingFrame == 0 && g_skinDumpRequested.exchange(false)) {
                if (!g_skinDump.readback) {
                    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
                    D3D12_RESOURCE_DESC rd{};
                    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    rd.Width = kSkinStateBytes;
                    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
                    rd.SampleDesc.Count = 1;
                    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    device5->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                        IID_PPV_ARGS(&g_skinDump.readback));
                }
                if (g_skinDump.readback) {
                    dumpNow = true;
                    g_skinDump.pendingFrame = frame;
                    g_skinDump.rows.clear();
                    for (auto* s : active) {
                        float dist = -1.0f;
                        for (const auto& c : cands) if (c.instance == s->instance) { dist = c.dist; break; }
                        g_skinDump.rows.push_back({ static_cast<std::uint32_t>(s - g_skin.slots), s->instance, s->mesh, s->vcount, s->icount / 3, dist });
                    }
                }
            }
            // Skin, then build every active slot's BLAS (each with its own scratch range).
            list4->SetComputeRootSignature(g_skin.rootSig.Get());
            list4->SetPipelineState(g_skin.pso.Get());
            list4->SetComputeRootShaderResourceView(1, inst.gpuva);
            list4->SetComputeRootShaderResourceView(2, lods.gpuva);
            list4->SetComputeRootShaderResourceView(3, meshes.gpuva);
            list4->SetComputeRootShaderResourceView(4, tr.gpuva);
            list4->SetComputeRootShaderResourceView(5, pool.gpuva);
            const UINT transformCount = static_cast<UINT>(tr.sizeBytes / 48);
            list4->SetComputeRootUnorderedAccessView(8, g_skin.slotState->GetGPUVirtualAddress());  // 0.5.50
            list4->SetComputeRootConstantBufferView(9, perScene);
            for (auto* s : active) {
                const UINT slot = static_cast<UINT>(s - g_skin.slots);
                const UINT c[9] = { s->instance, s->mesh, s->vcount, transformCount, s->icount, s->firstIndex, slot,
                                    static_cast<UINT>(frame + 1), dumpNow ? 1u : 0u };  // 0.5.51: frame never 0; 0.5.53: dump
                list4->SetComputeRoot32BitConstants(0, 9, c, 0);
                list4->SetComputeRootUnorderedAccessView(6, s->vb->GetGPUVirtualAddress());
                list4->SetComputeRootUnorderedAccessView(7, s->ib->GetGPUVirtualAddress());
                rt::DredMark(list4, L"SFRT Blas skin #3");
                list4->Dispatch(((std::max)(s->vcount, s->icount) + 63) / 64, 1, 1);
            }
            if (dumpNow) {  // 0.5.53
                D3D12_RESOURCE_BARRIER sb{};
                sb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                sb.Transition = { g_skin.slotState.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
                list4->ResourceBarrier(1, &sb);
                list4->CopyBufferRegion(g_skinDump.readback.Get(), 0, g_skin.slotState.Get(), 0, kSkinStateBytes);
                std::swap(sb.Transition.StateBefore, sb.Transition.StateAfter);
                list4->ResourceBarrier(1, &sb);
            }
            std::vector<D3D12_RESOURCE_BARRIER> bar(active.size() * 2);
            for (std::size_t i = 0; i < bar.size(); ++i) {
                bar[i] = {};
                bar[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bar[i].Transition = { (i & 1 ? active[i / 2]->ib : active[i / 2]->vb).Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
            }
            list4->ResourceBarrier(static_cast<UINT>(bar.size()), bar.data());
            for (auto* s : active) {
                D3D12_RAYTRACING_GEOMETRY_DESC geom{};
                geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geom.Triangles.VertexCount = s->vcount;
                geom.Triangles.VertexBuffer = { s->vb->GetGPUVirtualAddress(), 12 };
                geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                geom.Triangles.IndexCount = s->icount;
                geom.Triangles.IndexBuffer = s->ib->GetGPUVirtualAddress();
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
                b.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                b.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                b.Inputs.NumDescs = 1;
                b.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                b.Inputs.pGeometryDescs = &geom;
                b.DestAccelerationStructureData = s->blas->GetGPUVirtualAddress();
                b.ScratchAccelerationStructureData = g_skin.scratch->GetGPUVirtualAddress() + s->scratchOffset;
                rt::DredMark(list4, L"SFRT Blas AS build #4");
                list4->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
                s->builtThisFrame = true;
                ++g_skin.built;
            }
            for (auto& b : bar) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            list4->ResourceBarrier(static_cast<UINT>(bar.size()), bar.data());
            writeTable();
            if (frame - g_skin.lastLog >= 600) {
                g_skin.lastLog = frame;
                REX::INFO("Starfield RT: skinned characters: {} candidates within {} units, {} in the RT scene, {} skinned BLAS builds so far",
                    cands.size(), g_settings.skinRadius, active.size(), g_skin.built);
            }
        }

        bool CreateInstancePipeline(ID3D12Device* device)
        {
            D3D12_ROOT_PARAMETER p[14]{};
            p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants = { 0, 0, 16 };  // 0.5.100: + MinAngle; 0.5.126: + UseSolidBits; 0.5.134: + UsePhantomBits
            for (UINT i = 0; i < 4; ++i) {
                p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                p[1 + i].Descriptor = { i, 0 };
            }
            p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[5].Descriptor = { 0, 0 };
            p[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t4 materials (0.1.23)
            p[6].Descriptor = { 4, 0 };
            p[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t5 vegetation materials (0.1.23)
            p[7].Descriptor = { 5, 0 };
            p[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u1 active-instance counter (0.2.2)
            p[8].Descriptor = { 1, 0 };
            p[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b1 PerSceneConstants (0.2.3)
            p[9].Descriptor = { 1, 0 };
            p[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t6 skin slot table (0.2.32)
            p[10].Descriptor = { 6, 0 };
            p[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u2 skinned candidates (0.2.32)
            p[11].Descriptor = { 2, 0 };
            p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u3 drawn-solid bits (0.5.126)
            p[12].Descriptor = { 3, 0 };
            p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u4 phantom bits (0.5.134)
            p[13].Descriptor = { 4, 0 };
            for (auto& q : p) q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            const D3D12_ROOT_SIGNATURE_DESC desc{ 14, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> blob, error;
            if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
                FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_tlas.rootSig)))) {
                REX::WARN("Starfield RT: TLAS instance root signature failed");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_tlas.rootSig.Get();
            pd.CS = { g_BuildInstancesCS, sizeof(g_BuildInstancesCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_tlas.pso)))) {
                REX::WARN("Starfield RT: TLAS instance pipeline failed");
                return false;
            }
            // 0.2.2: active-instance counter and its read-back ring.
            g_tlas.counter = CreateGpuBuffer(device, kCounterAllocBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);  // 0.5.66: + near dump
            D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 256;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            for (std::uint32_t i = 0; i < kRing; ++i) {
                void* m = nullptr;
                if (FAILED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                        IID_PPV_ARGS(&g_tlas.counterReadback[i]))) ||
                    FAILED(g_tlas.counterReadback[i]->Map(0, nullptr, &m)) || !m) {
                    REX::WARN("Starfield RT: TLAS counter read-back failed");
                    return false;
                }
                g_tlas.counterMapped[i] = static_cast<const std::uint32_t*>(m);
            }
            return g_tlas.counter != nullptr;
        }

        bool EnsureAddressRing(ID3D12Device* device, std::uint32_t meshes, std::uint64_t frame)
        {
            if (meshes <= g_tlas.addrCapacity) return true;
            const std::uint32_t cap = (meshes + 4095) & ~4095u;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = std::uint64_t(cap) * 8;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            for (std::uint32_t i = 0; i < kRing; ++i) {
                if (g_tlas.addrRing[i]) g_graveyard.emplace_back(frame, std::move(g_tlas.addrRing[i]));
                g_tlas.addrMapped[i] = nullptr;
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                        nullptr, IID_PPV_ARGS(&g_tlas.addrRing[i])))) {
                    return false;
                }
                const D3D12_RANGE none{ 0, 0 };
                void* p = nullptr;
                if (FAILED(g_tlas.addrRing[i]->Map(0, &none, &p))) return false;  // stays mapped (upload heap)
                g_tlas.addrMapped[i] = static_cast<std::uint64_t*>(p);
            }
            g_tlas.addrCapacity = cap;
            return true;
        }

        bool EnsureTlasBuffers(ID3D12Device5* device5, std::uint32_t instances, std::uint64_t frame)
        {
            if (instances <= g_tlas.capacity && g_tlas.result) return true;
            const std::uint32_t requested = instances;  // 0.5.124: logged
            // 0.2.30: grow geometrically (x1.5, in 16384-instance steps). During a load the requested count rose 1024 at a
            // time, so the ~110 MB of TLAS buffers were reallocated several times a second while each old set stayed alive
            // for kGraveyardFrames: close to 1 GB in flight right before both load hangs (0.2.22, 0.2.29).
            {
                // 0.5.5: first allocation is already 131072 instances (~60 MB), above the 98304 a Neon load asks for, so the
                // TLAS buffers are no longer swapped in the middle of a load.
                // 0.5.124: 262144 (about 120 MB). Both 0.5.122/0.5.123 Frame Generation hangs came 4-5 s after the TLAS
                // buffers grew 131072 -> 196608 in play (one when a menu opened, one 11 s after a menu closed): the active
                // count plus 25% headroom crossed 131072. Allocating enough up front keeps that swap out of play.
                constexpr std::uint64_t kFirstTlasCapacity = 262144;
                const std::uint64_t grown = (std::max<std::uint64_t>)((std::max<std::uint64_t>)(instances, std::uint64_t(g_tlas.capacity) * 3 / 2),
                    g_tlas.capacity ? 0 : kFirstTlasCapacity);
                const std::uint64_t cap = (std::max<std::uint64_t>)(static_cast<std::uint64_t>((std::max)(g_settings.tlasMaxInstances, 1024)), instances);
                instances = static_cast<std::uint32_t>((std::min)((grown + 16383) & ~16383ull, cap));
            }
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = instances;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
            // 0.3.7: a reallocation soon after a load means the scene is still filling in: keep the effects paused a bit longer.
            if (frame < g_quietUntil.load() + 300) {
                const std::uint64_t until = frame + WaitFrames(90, 1.0f, 30);  // 0.5.103: was 90 frames
                if (until > g_quietUntil.load()) g_quietUntil = until;
            }
            ComPtr<ID3D12Resource>* old[] = { &g_tlas.descs, &g_tlas.result, &g_tlas.scratch };
            for (auto* r : old) {
                if (*r) g_graveyard.emplace_back(frame, std::move(*r));
            }
            g_tlas.descs = CreateGpuBuffer(device5, std::uint64_t(instances) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_tlas.result = CreateGpuBuffer(device5, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            g_tlas.scratch = CreateGpuBuffer(device5, info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!g_tlas.descs || !g_tlas.result || !g_tlas.scratch) {
                REX::WARN("Starfield RT: TLAS buffer allocation failed ({} instances)", instances);
                g_tlas.capacity = 0;
                return false;
            }
            g_tlas.capacity = instances;
            g_tlas.resultBytes = info.ResultDataMaxSizeInBytes;
            g_tlas.scratchBytes = info.ScratchDataSizeInBytes;
            REX::INFO("Starfield RT: TLAS buffers for {} instances: result {:.1f} MB, scratch {:.1f} MB, descriptors {:.1f} MB (asked for {}, "
                      "{} active at the last count, frame {})", instances, info.ResultDataMaxSizeInBytes / 1048576.0,
                info.ScratchDataSizeInBytes / 1048576.0, instances * 64 / 1048576.0, requested, g_tlas.activeCount, frame);
            return true;
        }

        // Records: BLAS address table, instance-descriptor dispatch, TLAS build. Caller restores compute state.
        bool BuildTlas(ID3D12Device5* device5, ID3D12GraphicsCommandList4* list4, D3D12_GPU_DESCRIPTOR_HANDLE table,
            const BufferView& meshes, D3D12_GPU_VIRTUAL_ADDRESS perScene, std::uint64_t frame)
        {
            BufferView inst{}, lods{};
            if (!LookupGpuTable(table, kTableInstances, inst) || !LookupGpuTable(table, kTableLods, lods) ||
                inst.stride != 96 || lods.stride != 20) {
                return false;
            }
            if (!g_tlas.pso && !g_tlas.pipelineFailed) {
                g_tlas.pipelineFailed = !CreateInstancePipeline(device5);
            }
            if (!g_tlas.pso) return false;

            const std::uint32_t meshCount = static_cast<std::uint32_t>(meshes.sizeBytes / kMeshStride);
            const std::uint32_t records = static_cast<std::uint32_t>(inst.sizeBytes / 96);
            const std::uint32_t instances = (std::min)(records, static_cast<std::uint32_t>((std::max)(g_settings.tlasMaxInstances, 1024)));
            // 0.2.3: NumDescs from the active count read back kRing frames ago (+25% and 4096 headroom); the TLAS
            // buffers are sized for that, not for the scan range, and grow when the count does.
            const std::uint32_t slot = static_cast<std::uint32_t>(frame % kRing);
            if (g_tlas.countFrames >= kRing && g_tlas.counterMapped[slot]) {
                g_tlas.activeCount = g_tlas.counterMapped[slot][0];
                g_tlas.glassCount = g_tlas.counterMapped[slot][1];  // 0.2.44
                g_tlas.playerCount = g_tlas.counterMapped[slot][2];  // 0.2.67
                g_tlas.sizeCulled = g_tlas.counterMapped[slot][3];   // 0.5.100
                for (int b = 0; b < 11; ++b) g_tlas.breakdown[b] = g_tlas.counterMapped[slot][5 + b];  // 0.5.101
                g_tlas.dupMesh = g_tlas.counterMapped[slot][16];   // 0.5.102
                g_tlas.dupPlace = g_tlas.counterMapped[slot][17];
            }
            std::uint32_t limit = (std::min)(instances, 65536u);
            if (g_tlas.countFrames >= kRing) {
                const std::uint64_t want = std::uint64_t(g_tlas.activeCount) + g_tlas.activeCount / 4 + 4096;
                limit = static_cast<std::uint32_t>((std::min<std::uint64_t>)(instances, (want + 1023) & ~1023ull));
            }
            if (!EnsureAddressRing(device5, meshCount, frame) || !EnsureTlasBuffers(device5, limit, frame)) {
                return false;
            }

            // 0.2.32: skinned characters (their BLAS builds are covered by the UAV barrier below).
            if (!g_skin.ready && !g_skin.failed) {
                g_skin.ready = CreateSkinResources(device5);
                g_skin.failed = !g_skin.ready;
            }
            const bool skinOn = g_skin.ready && perScene != 0;
            if (skinOn) UpdateSkinned(device5, list4, table, meshes, perScene, frame);

            // BLAS address per mesh index for this frame.
            std::uint64_t* addr = g_tlas.addrMapped[frame % kRing];
            for (std::uint32_t i = 0; i < meshCount; ++i) {
                addr[i] = (i < g_entries.size() && g_entries[i].built && g_entries[i].va) ? g_entries[i].va : 0;
            }

            D3D12_RESOURCE_BARRIER all{};
            all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // null = every UAV write, including this frame's BLAS builds
            list4->ResourceBarrier(1, &all);

            // 0.1.23: materials decide which instances are alpha tested.
            BufferView materials{};
            const bool haveMaterials = LookupGpuTable(table, kTableMaterials, materials) && materials.resource && materials.stride == 288;
            const UINT materialCount = haveMaterials ? static_cast<UINT>(materials.sizeBytes / 288) : 0;
            BufferView veg{};
            const bool haveVeg = LookupGpuTable(table, kTableVegMaterials, veg) && veg.resource && veg.stride == 368;
            const UINT vegCount = haveVeg ? static_cast<UINT>(veg.sizeBytes / 368) : 0;
            g_tlas.limit = limit;
            const float cull = perScene ? static_cast<float>((std::max)(g_settings.tlasCullDistance, 0)) : 0.0f;
            UINT consts[16]{ instances, records, static_cast<UINT>(lods.sizeBytes / 20), meshCount, 0, materialCount,
                (g_settings.alphaTest ? 1u : 0u) | (g_settings.glassSeparate ? 2u : 0u), vegCount, 0u, limit, 0u, 0u,
                skinOn ? (std::min)(static_cast<UINT>((std::max)(g_settings.skinSlots, 0)), static_cast<UINT>(kSkinSlotsMax)) : 0u };
            std::memcpy(&consts[10], &cull, 4);
            const float skinRadius = skinOn ? g_settings.skinRadius : 0.0f;
            std::memcpy(&consts[11], &skinRadius, 4);
            const float maxExtent = static_cast<float>((std::max)(g_settings.tlasMaxExtent, 1));
            std::memcpy(&consts[4], &maxExtent, 4);
            if (g_tlas.builds % 600 == 0) {
                REX::INFO("Starfield RT: materials: standard (+{}) {} entries{}, vegetation (+{}) {} entries{}, alpha test {}",
                    kTableMaterials, materialCount, haveMaterials ? "" : " (NOT FOUND)", kTableVegMaterials, vegCount,
                    haveVeg ? "" : " (NOT FOUND)", g_settings.alphaTest ? "on" : "off");
            }
            list4->SetComputeRootSignature(g_tlas.rootSig.Get());
            list4->SetPipelineState(g_tlas.pso.Get());
            // 0.5.100: size-on-screen cull, tightened (with the range) while VRAM is nearly full.
            const float pressure = VramPressureScale(device5, frame);
            const float minAngle = perScene && g_settings.tlasMinAngle > 0.0f ? g_settings.tlasMinAngle / pressure : 0.0f;
            std::memcpy(&consts[13], &minAngle, 4);
            if (pressure < 1.0f) {
                const float cullP = cull * pressure;
                std::memcpy(&consts[10], &cullP, 4);
            }
            const D3D12_GPU_VIRTUAL_ADDRESS solidBits = GlassSolidBits();  // 0.5.126
            consts[14] = solidBits ? 1u : 0u;
            const D3D12_GPU_VIRTUAL_ADDRESS phantomBits = PhantomBits();  // 0.5.134
            consts[15] = phantomBits ? 1u : 0u;
            list4->SetComputeRoot32BitConstants(0, 16, consts, 0);
            list4->SetComputeRootUnorderedAccessView(13, phantomBits ? phantomBits : g_tlas.counter->GetGPUVirtualAddress());  // never read when 0
            list4->SetComputeRootUnorderedAccessView(12, solidBits ? solidBits : g_tlas.counter->GetGPUVirtualAddress());  // never read when 0
            list4->SetComputeRootConstantBufferView(9, perScene ? perScene : inst.gpuva);  // never read when cull is 0
            list4->SetComputeRootShaderResourceView(1, inst.gpuva);
            list4->SetComputeRootShaderResourceView(2, lods.gpuva);
            list4->SetComputeRootShaderResourceView(3, meshes.gpuva);
            list4->SetComputeRootShaderResourceView(4, g_tlas.addrRing[frame % kRing]->GetGPUVirtualAddress());
            list4->SetComputeRootUnorderedAccessView(5, g_tlas.descs->GetGPUVirtualAddress());
            list4->SetComputeRootShaderResourceView(6, haveMaterials ? materials.gpuva : lods.gpuva);  // never read when count is 0
            list4->SetComputeRootShaderResourceView(7, haveVeg ? veg.gpuva : lods.gpuva);
            list4->SetComputeRootUnorderedAccessView(8, g_tlas.counter->GetGPUVirtualAddress());
            // 0.2.32: skinned slots and candidates (stand-ins when skinning is unavailable: never read with 0 slots / radius 0).
            list4->SetComputeRootShaderResourceView(10, g_skin.ready ? g_skin.table[frame % kRing]->GetGPUVirtualAddress() : lods.gpuva);
            list4->SetComputeRootUnorderedAccessView(11, g_skin.ready ? g_skin.cands->GetGPUVirtualAddress() : g_tlas.counter->GetGPUVirtualAddress());
            D3D12_RESOURCE_BARRIER cuav{};
            cuav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            // Pass 0: reset the counter (0.5.102: and clear the duplicate hash tables). Pass 1: pack active instances.
            // Pass 2: null the tail up to the limit.
            rt::DredMark(list4, L"SFRT Blas tlas #5");
            list4->Dispatch((2 * (1u << 19)) / 64, 1, 1);
            list4->ResourceBarrier(1, &cuav);
            consts[8] = 1;
            list4->SetComputeRoot32BitConstants(0, 1, &consts[8], 8);
            rt::DredMark(list4, L"SFRT Blas tlas #6");
            list4->Dispatch((instances + 63) / 64, 1, 1);
            list4->ResourceBarrier(1, &cuav);
            consts[8] = 2;
            list4->SetComputeRoot32BitConstants(0, 1, &consts[8], 8);
            rt::DredMark(list4, L"SFRT Blas tlas #7");
            list4->Dispatch((limit + 63) / 64, 1, 1);
            list4->ResourceBarrier(1, &cuav);
            // Copy this frame's active count for the CPU (read kRing frames later).
            D3D12_RESOURCE_BARRIER ctr{};
            ctr.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ctr.Transition = { g_tlas.counter.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE };
            list4->ResourceBarrier(1, &ctr);
            list4->CopyBufferRegion(g_tlas.counterReadback[slot].Get(), 0, g_tlas.counter.Get(), 0, 64);  // 0.2.44 glass, 0.2.67 player, 0.5.100 size-culled, 0.5.101 breakdown
            list4->CopyBufferRegion(g_tlas.counterReadback[slot].Get(), 64, g_tlas.counter.Get(), 4088, 8);  // 0.5.102 duplicates
            if (g_nearDumpFrame && frame >= g_nearDumpFrame + kRing + 3 && g_nearDumpReadback) {  // 0.5.66
                g_nearDumpFrame = 0;
                void* m = nullptr;
                const D3D12_RANGE rr{ 0, kCounterBytes };
                if (SUCCEEDED(g_nearDumpReadback->Map(0, &rr, &m)) && m) {
                    LogNearDump(static_cast<const std::uint32_t*>(m));
                    const D3D12_RANGE none{ 0, 0 };
                    g_nearDumpReadback->Unmap(0, &none);
                }
            }
            if (!g_nearDumpFrame && g_nearDumpRequested.exchange(false)) {
                if (!g_nearDumpReadback) {
                    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                    D3D12_RESOURCE_DESC bd{};
                    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    bd.Width = kCounterBytes;
                    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                    bd.SampleDesc.Count = 1;
                    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    device5->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                        IID_PPV_ARGS(&g_nearDumpReadback));
                }
                if (g_nearDumpReadback) {
                    list4->CopyBufferRegion(g_nearDumpReadback.Get(), 0, g_tlas.counter.Get(), 0, kCounterBytes);
                    g_nearDumpFrame = (std::max)(frame, std::uint64_t(1));
                }
            }
            ctr.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            ctr.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            list4->ResourceBarrier(1, &ctr);
            ++g_tlas.countFrames;
            if (skinOn) {  // 0.2.32: this frame's skinned candidates, for slot assignment kRing frames later
                D3D12_RESOURCE_BARRIER sc{};
                sc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                sc.Transition = { g_skin.cands.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE };
                list4->ResourceBarrier(1, &sc);
                list4->CopyBufferRegion(g_skin.candsReadback[slot].Get(), 0, g_skin.cands.Get(), 0, kSkinCandBytes);
                std::swap(sc.Transition.StateBefore, sc.Transition.StateAfter);
                list4->ResourceBarrier(1, &sc);
                ++g_skin.frames;
            }

            D3D12_RESOURCE_BARRIER toRead{};
            toRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRead.Transition = { g_tlas.descs.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
            list4->ResourceBarrier(1, &toRead);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            build.Inputs.NumDescs = limit;  // 0.2.2: only the compacted range
            build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            build.Inputs.InstanceDescs = g_tlas.descs->GetGPUVirtualAddress();
            build.DestAccelerationStructureData = g_tlas.result->GetGPUVirtualAddress();
            build.ScratchAccelerationStructureData = g_tlas.scratch->GetGPUVirtualAddress();
            rt::DredMark(list4, L"SFRT Blas AS build #8");
            list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

            D3D12_RESOURCE_BARRIER after[2]{};
            after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            after[0].UAV.pResource = g_tlas.result.Get();
            after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            after[1].Transition = { g_tlas.descs.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            list4->ResourceBarrier(2, after);

            g_tlasAddress = g_tlas.result->GetGPUVirtualAddress();
            g_tlas.lastInstances = limit;
            ++g_tlas.builds;
            return true;
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetTlasAddress() { return g_tlasAddress.load(); }

    bool AlphaTestOn() { return g_settings.alphaTest; }

    void RequestTableDump(std::uint32_t family, std::uint32_t material)
    {
        g_tableDumpReq = 0x80000000u | ((family & 0x1Fu) << 16) | (material & 0x7FFFu);
    }

    bool SceneLoading() { return g_sceneLoading.load(); }
    bool SceneSettling() { return g_sceneLoading.load() || g_lastFrame.load() < g_quietUntil.load(); }

    float RtFadeIn()
    {
        if (SceneSettling()) return 0.0f;
        const long long end = g_settleEndNs.load();
        if (end == 0) return 0.0f;
        const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return (std::clamp)(static_cast<float>(now - end) / ((std::max)(g_fadeSeconds, 0.01f) * 1e9f), 0.0f, 1.0f);
    }

    void SetLoadTiming(float settleSeconds, float fadeSeconds)
    {
        g_settleSeconds = (std::clamp)(settleSeconds, 0.0f, 3.0f);
        g_fadeSeconds = (std::clamp)(fadeSeconds, 0.0f, 10.0f);
    }

    bool ToggleGlassSeparate()
    {
        std::lock_guard lock(g_lock);
        g_settings.glassSeparate = !g_settings.glassSeparate;
        return g_settings.glassSeparate;
    }

    bool ToggleAlphaTest()
    {
        g_settings.alphaTest = !g_settings.alphaTest;
        return g_settings.alphaTest;
    }

    void RequestSkinDump() { g_skinDumpRequested = true; g_nearDumpRequested = true; }  // 0.5.66: + near-camera dump

    void SetBlasSettings(const BlasSettings& s)
    {
        std::lock_guard lock(g_lock);
        g_settings = s;
    }

    bool BlasFrameUpdate(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_GPU_DESCRIPTOR_HANDLE table,
        D3D12_GPU_VIRTUAL_ADDRESS perScene, std::uint64_t frame)
    {
        std::lock_guard lock(g_lock);
        if (!g_settings.enabled || !device || !list || !table.ptr || !DeviceHooksReady()) {
            return false;
        }
        // Release buffers retired long enough ago that the GPU can no longer be using them.
        std::erase_if(g_graveyard, [frame](const auto& g) { return frame - g.first > kGraveyardFrames; });
        ReleasePendingFrees(frame);  // 0.2.7: BLAS ranges retired long enough ago go back to their page

        BufferView meshes{}, pool{};
        if (!LookupGpuTable(table, kTableMeshes, meshes) || !LookupGpuTable(table, kTablePool, pool) || !meshes.resource || !pool.resource || meshes.stride != kMeshStride) {
            return false;
        }
        const bool meshBufferChanged = meshes.gpuva != g_lastMeshVa || meshes.sizeBytes != g_lastMeshSize;
        if (meshBufferChanged || frame - g_lastScan >= std::uint64_t((std::max)(g_settings.scanInterval, 1))) {
            ScanMeshes(meshes, table, frame);
            g_lastScan = frame;
            g_lastMeshVa = meshes.gpuva;
            g_lastMeshSize = meshes.sizeBytes;
        }

        // 0.5.111: scene table dump for an unknown material family (requested by the crosshair probe).
        if (const std::uint32_t req = g_tableDumpReq.exchange(0); req & 0x80000000u) {
            const std::uint32_t family = (req >> 16) & 0x1Fu, material = req & 0x7FFFu;
            REX::INFO("Starfield RT: [tables] family {} material {}: entry {} of every CPU-readable structured buffer in the scene table",
                family, material, material);
            for (std::uint32_t slot = 0; slot < 64; ++slot) {
                BufferView v{};
                if (!LookupGpuTableAny(table, slot, v)) continue;
                if (v.cbv || !v.resource) {
                    REX::INFO("Starfield RT: [tables]   +{}: {} ({} bytes)", slot, v.cbv ? "CBV" : "texture or null", v.sizeBytes);
                    continue;
                }
                const std::uint32_t stride = v.stride ? v.stride : 0u;
                std::string words;
                bool mapped = false;
                if (stride && std::uint64_t(material + 1) * stride <= v.sizeBytes) {
                    void* m = nullptr;
                    const std::uint64_t base = v.gpuva - v.resource->GetGPUVirtualAddress() + std::uint64_t(material) * stride;
                    const std::uint32_t n = (std::min)(stride, 160u) / 4;
                    const D3D12_RANGE rr{ static_cast<SIZE_T>(base), static_cast<SIZE_T>(base + n * 4) };
                    if (SUCCEEDED(v.resource->Map(0, &rr, &m)) && m) {
                        mapped = true;
                        const auto* w = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(m) + base);
                        for (std::uint32_t i = 0; i < n; ++i) words += std::format("{}{:08x}", i ? " " : "", w[i]);
                        const D3D12_RANGE none{ 0, 0 };
                        v.resource->Unmap(0, &none);
                    }
                }
                REX::INFO("Starfield RT: [tables]   +{}: stride {}, {} bytes ({} entries){}{}", slot, stride, v.sizeBytes,
                    stride ? v.sizeBytes / stride : 0, mapped ? ": " : (stride ? " (not CPU-readable or too small)" : " (raw)"), words);
            }
        }

        bool recorded = false;
        ComPtr<ID3D12Device5> device5;
        ComPtr<ID3D12GraphicsCommandList4> list4;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || FAILED(list->QueryInterface(IID_PPV_ARGS(&list4)))) {
            return false;
        }
        if (!g_queue.empty()) {
            void* mapped = nullptr;
            const std::uint64_t offset = meshes.gpuva - meshes.resource->GetGPUVirtualAddress();
            const D3D12_RANGE readRange{ static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + meshes.sizeBytes) };
            if (SUCCEEDED(meshes.resource->Map(0, &readRange, &mapped)) && mapped) {
                const std::uint64_t before = g_built;
                BuildQueued(device5.Get(), list4.Get(), table, pool, static_cast<const std::uint8_t*>(mapped) + offset, meshes.sizeBytes, frame);
                recorded = g_built != before;
                const D3D12_RANGE noWrite{ 0, 0 };
                meshes.resource->Unmap(0, &noWrite);
            }
        }
        // 0.2.12: the scene counts as loading while many BLASes are queued or records are settling.
        // 0.2.23: with hysteresis. Loading starts above 1000 queued/settling, but only ends once both stay under 200
        // and the TLAS has not had to grow for 90 frames in a row (0.2.22 hung 0.4 s after resuming at 984 queued,
        // with the TLAS still growing every few frames).
        {
            // 0.5.103: frame rate for the waits below, and how long RT effects were off after each load.
            {
                using clock = std::chrono::steady_clock;
                static clock::time_point s_prev{};
                const auto now = clock::now();
                if (s_prev.time_since_epoch().count() != 0) {
                    const float dt = std::chrono::duration<float>(now - s_prev).count();
                    if (dt > 0.0f && dt < 0.5f) g_fps += (1.0f / dt - g_fps) * 0.05f;
                }
                s_prev = now;
                static bool s_wasSettling = false;
                static clock::time_point s_offSince{};
                const bool settling = SceneSettling();
                if (settling && !s_wasSettling) s_offSince = now;
                if (settling) g_settleEndNs = 0;  // 0.5.104
                else if (s_wasSettling || g_settleEndNs.load() == 0)
                    g_settleEndNs = (std::max)(1ll, static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
                if (!settling && s_wasSettling)
                    REX::INFO("Starfield RT: RT effects back after {:.1f} s (loading and settling, {:.0f} fps rendered)",
                        std::chrono::duration<float>(now - s_offSince).count(), g_fps);
                s_wasSettling = settling;
            }
            static std::uint64_t s_calmSince = 0;
            static std::uint32_t s_lastCapacity = 0;
            const bool busy = g_queue.size() > 1000 || g_scanStats.settling > 1000;
            const bool calm = g_queue.size() < 200 && g_scanStats.settling < 200 && g_tlas.capacity == s_lastCapacity;
            s_lastCapacity = g_tlas.capacity;
            if (!calm) s_calmSince = frame;
            bool loading = g_sceneLoading.load();
            if (busy) loading = true;
            else if (loading && frame - s_calmSince >= WaitFrames(90, 1.0f, 30)) loading = false;  // 0.5.103: was 90 frames
            g_lastFrame = frame;
            if (loading != g_sceneLoading.exchange(loading)) {
                ForgetReflectionInputs();  // 0.2.63: inputs remembered from before (or during) the load point at freed buffers
                g_quietUntil = frame + WaitFrames(180, g_settleSeconds, 45);  // 0.3.7: 180 frames; 0.5.103: at most 2 s (at least 45 frames); 0.5.106: ini
                REX::INFO("Starfield RT: scene {} ({} BLAS builds queued, {} records settling): RT shadows {}", loading ? "loading" : "ready",
                    g_queue.size(), g_scanStats.settling, loading ? "paused" : "resumed");
            }
        }
        timer::Stamp(list4.Get(), timer::kAfterBlas);
        if (g_settings.tlasEnabled) {
            recorded |= BuildTlas(device5.Get(), list4.Get(), table, meshes, perScene, frame);
        }
        timer::Stamp(list4.Get(), timer::kAfterTlas);

        if (frame - g_lastLog >= 600) {  // about every 10 s
            g_lastLog = frame;
            std::size_t live = 0;
            for (const auto& e : g_entries) live += e.built;
            REX::INFO("Starfield RT: BLAS: {} live ({:.1f} MB), {} built total, {} queued, {} failed, {} skipped, {} stale, "
                      "{} out of bounds, scratch {:.1f} MB, mesh table {} slots",
                live, g_totalBytes / 1048576.0, g_built, g_queue.size(), g_failed, g_skipped, g_stale, g_outOfBounds,
                g_scratchSize / 1048576.0, meshes.sizeBytes / kMeshStride);
            REX::INFO("Starfield RT: BLAS pool: {} pages, {:.0f} MB reserved, {:.1f} MB in use; {} in-place replacements, {} slot reuses",
                g_pages.size(), g_pageBytes / 1048576.0, g_totalBytes / 1048576.0, g_replacements, g_slotReuse);
            REX::INFO("Starfield RT: meshes: {} in use: {} with positions in the pool, {} in separate buffers ({}), {} skinned, {} over 65536 vertices; {} settling; {} streamed-buffer builds held",
                g_scanStats.used, g_scanStats.inPool, g_scanStats.outOfPool, g_settings.poolOnly ? "SKIPPED: bBlasPoolOnly=1" : "built",
                g_scanStats.skinned, g_scanStats.tooBig, g_scanStats.settling, g_retainedStreamed);
            if (g_settings.tlasEnabled) {
                REX::INFO("Starfield RT: TLAS: {} builds, {} active instances ({} glass{}, {} first-person player parts; {} left out as too small on screen, VRAM range x{:.2f}), {} descriptors built (capacity {}), result {:.1f} MB, gpuva {:#x}",
                    g_tlas.builds, g_tlas.activeCount, g_tlas.glassCount, g_settings.glassSeparate ? ", skipped by reflection rays" : ", separation off",
                    g_tlas.playerCount, g_tlas.sizeCulled, g_vramScale,
                    g_tlas.lastInstances, g_tlas.capacity, g_tlas.resultBytes / 1048576.0, g_tlasAddress.load());
                const auto& b = g_tlas.breakdown;
                REX::INFO("Starfield RT: TLAS breakdown: distance <50 m {} | 50-200 {} | 200-1000 {} | 1000+ {}; radius <0.5 m {} | 0.5-2 {} | 2-10 {} | 10+ {}; alpha-tested {}, effect meshes {}, characters {}; duplicates: {} same mesh at the same place, {} sharing a place with another object",
                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], g_tlas.dupMesh, g_tlas.dupPlace);
            }
        }
        return recorded;
    }
}
