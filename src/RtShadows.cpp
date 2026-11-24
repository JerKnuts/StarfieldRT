// Ray-traced sun shadows (see RtShadows.h, shaders/RtSunShadow.hlsl).

#include <SFSE/SFSE.h>  // CommonLibSF must come before any Windows headers

#include "RtShadows.h"
#include "generated/RtProbeCS.h"
#include "generated/RtSunShadowCS.h"
#include "generated/RtSunDenoiseCS.h"
#include "GpuTimer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <vector>
#include <string>

#include <wrl/client.h>

#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace rt
{
    namespace
    {
        SunShadowSettings           g_settings;
        ComPtr<ID3D12RootSignature> g_rootSig;
        ComPtr<ID3D12PipelineState> g_pso;
        ComPtr<ID3D12PipelineState> g_denoisePso;      // 0.1.30: soft shadow filter (pass 2)
        ComPtr<ID3D12Resource>      g_visBuffer;       // 0.1.30: per-pixel pass-1 results, 8 B per pixel
        std::uint64_t               g_visBytes = 0;
        std::vector<ComPtr<ID3D12Resource>> g_oldVisBuffers;  // kept alive after a resize (may still be in flight)
        std::atomic<bool>           g_softLive{ true };
        std::once_flag              g_once;
        bool                        g_ready = false;

        bool CreatePipeline(ID3D12Device* device)
        {
            // 0.1.23: root signature 1.1 with CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, so the alpha test can read the
            // engine's bindless streams and textures (SM 6.6 ResourceDescriptorHeap) like the game's shaders do.
            // Volatile flags keep the 1.0 semantics (the engine's tables and buffers change between frames).
            D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
                { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0 },  // depth at table +0
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 2 },  // mask at table +2
            };
            D3D12_ROOT_PARAMETER1 p[12]{};
            p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants = { 2, 0, 14 };
            auto descriptor = [](D3D12_ROOT_PARAMETER1& q, D3D12_ROOT_PARAMETER_TYPE type, UINT reg, UINT space) {
                q.ParameterType = type;
                q.Descriptor = { reg, space, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            };
            descriptor(p[1], D3D12_ROOT_PARAMETER_TYPE_CBV, 0, 0);
            descriptor(p[2], D3D12_ROOT_PARAMETER_TYPE_CBV, 1, 0);
            descriptor(p[3], D3D12_ROOT_PARAMETER_TYPE_SRV, 0, 1);  // t0 space1: acceleration structure
            p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[4].DescriptorTable = { 2, ranges };
            descriptor(p[5], D3D12_ROOT_PARAMETER_TYPE_SRV, 1, 1);  // t1 space1: LOD entries
            descriptor(p[6], D3D12_ROOT_PARAMETER_TYPE_SRV, 2, 1);  // t2 space1: mesh records
            descriptor(p[7], D3D12_ROOT_PARAMETER_TYPE_SRV, 3, 1);  // t3 space1: standard materials
            descriptor(p[8], D3D12_ROOT_PARAMETER_TYPE_SRV, 4, 1);  // t4 space1: vegetation materials
            descriptor(p[9], D3D12_ROOT_PARAMETER_TYPE_SRV, 5, 1);  // t5 space1: alpha params (0.1.26)
            descriptor(p[10], D3D12_ROOT_PARAMETER_TYPE_SRV, 6, 1); // t6 space1: geometry pool indices (0.1.29)
            descriptor(p[11], D3D12_ROOT_PARAMETER_TYPE_UAV, 1, 1); // u1 space1: soft shadow buffer (0.1.30)
            for (auto& q : p) q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = 0;
            sampler.RegisterSpace = 1;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
            desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            desc.Desc_1_1 = { 12, p, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED };
            ComPtr<ID3DBlob> blob, error;
            if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
                REX::WARN("Starfield RT: sun shadow root signature: {}", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
                return false;
            }
            if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig)))) {
                REX::WARN("Starfield RT: sun shadow CreateRootSignature failed");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_rootSig.Get();
            pd.CS = { g_RtSunShadowCS, sizeof(g_RtSunShadowCS) };
            const HRESULT hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
            if (FAILED(hr)) {
                REX::WARN("Starfield RT: sun shadow pipeline failed ({:#x})", static_cast<unsigned>(hr));
                return false;
            }
            pd.CS = { g_RtSunDenoiseCS, sizeof(g_RtSunDenoiseCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_denoisePso)))) {
                REX::WARN("Starfield RT: soft shadow filter pipeline failed; soft shadows disabled");
                g_denoisePso.Reset();
            }
            REX::INFO("Starfield RT: sun shadow pipeline created");
            return true;
        }
    }

    namespace
    {
        std::atomic<int> g_liveMode{ -1 };  // -1 = follow settings; otherwise overrides mode

        // ---- debug probe ----
        // 0.1.19: sample 0 = screen centre, then a kGridX x kGridY grid over the screen.
        // 0.1.25: 1536 bytes per sample (alpha candidates and material dumps; layout in RtProbe.hlsl).
        constexpr UINT   kGridX = 16, kGridY = 9;
        constexpr UINT   kProbeSamples = 1 + kGridX * kGridY;
        constexpr UINT64 kSampleBytes = 1536;
        constexpr UINT64 kProbeBytes = kProbeSamples * kSampleBytes;
        ComPtr<ID3D12RootSignature> g_probeSig;
        ComPtr<ID3D12PipelineState> g_probePso;
        ComPtr<ID3D12Resource>      g_probeOut, g_probeReadback;
        std::once_flag              g_probeOnce;
        bool                        g_probeReady = false;
        std::atomic<bool>           g_probeRequested{ false };
        std::uint64_t               g_probePendingFrame = 0;  // 0 = nothing in flight

        ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES heap{ type };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = kProbeBytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = flags;
            ComPtr<ID3D12Resource> r;
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
            return r;
        }

        bool CreateProbe(ID3D12Device* device)
        {
            // 0.1.25: root signature 1.1 with heap-direct indexing, like the shadow pass (shared alpha test).
            D3D12_DESCRIPTOR_RANGE1 range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0 };  // depth at table +0
            D3D12_ROOT_PARAMETER1 p[13]{};
            p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants = { 2, 0, 12 };
            auto descriptor = [](D3D12_ROOT_PARAMETER1& q, D3D12_ROOT_PARAMETER_TYPE type, UINT reg, UINT space) {
                q.ParameterType = type;
                q.Descriptor = { reg, space, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            };
            descriptor(p[1], D3D12_ROOT_PARAMETER_TYPE_CBV, 0, 0);
            descriptor(p[2], D3D12_ROOT_PARAMETER_TYPE_CBV, 1, 0);
            descriptor(p[3], D3D12_ROOT_PARAMETER_TYPE_SRV, 0, 1);  // TLAS
            descriptor(p[4], D3D12_ROOT_PARAMETER_TYPE_SRV, 1, 0);  // instances
            descriptor(p[5], D3D12_ROOT_PARAMETER_TYPE_SRV, 2, 0);  // LOD entries
            descriptor(p[6], D3D12_ROOT_PARAMETER_TYPE_SRV, 3, 0);  // mesh records
            descriptor(p[7], D3D12_ROOT_PARAMETER_TYPE_UAV, 1, 0);  // output
            p[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[8].DescriptorTable = { 1, &range };
            descriptor(p[9], D3D12_ROOT_PARAMETER_TYPE_SRV, 4, 0);   // standard materials
            descriptor(p[10], D3D12_ROOT_PARAMETER_TYPE_SRV, 5, 0);  // vegetation materials
            descriptor(p[11], D3D12_ROOT_PARAMETER_TYPE_SRV, 6, 0);  // alpha params (0.1.26)
            descriptor(p[12], D3D12_ROOT_PARAMETER_TYPE_SRV, 7, 0);  // geometry pool indices (0.1.29)
            for (auto& q : p) q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.RegisterSpace = 1;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
            desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            desc.Desc_1_1 = { 13, p, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED };
            ComPtr<ID3DBlob> blob, error;
            if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
                FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_probeSig)))) {
                REX::WARN("Starfield RT: probe root signature failed");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_probeSig.Get();
            pd.CS = { g_RtProbeCS, sizeof(g_RtProbeCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_probePso)))) {
                REX::WARN("Starfield RT: probe pipeline failed");
                return false;
            }
            g_probeOut = MakeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_probeReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            return g_probeOut && g_probeReadback;
        }

        std::string Dwords(const std::uint8_t* p, std::size_t bytes)
        {
            std::string s;
            for (std::size_t i = 0; i < bytes; i += 4) {
                std::uint32_t v;
                std::memcpy(&v, p + i, 4);
                s += std::format("{}{:08x}", i ? " " : "", v);
            }
            return s;
        }

        // 0.1.25: the alpha-tested surfaces a sun ray passed through (same test as the shadow pass).
        void LogCandidates(const std::uint8_t* s, const char* where)
        {
            auto u = [s](std::size_t o) { std::uint32_t v; std::memcpy(&v, s + o, 4); return v; };
            auto f = [s](std::size_t o) { float v; std::memcpy(&v, s + o, 4); return v; };
            const std::uint32_t n = u(552);
            if (!n) return;
            REX::INFO("Starfield RT: PROBE   {} sun ray met {} alpha-tested surface(s){}", where, n, n > 4 ? " (first 4 logged)" : "");
            for (std::uint32_t i = 0; i < (std::min)(n, 4u); ++i) {
                const std::size_t c = 560 + i * 32;
                const std::uint32_t last = u(c + 28);
                const std::uint32_t tb = last & ~1u;
                float t;
                std::memcpy(&t, &tb, 4);
                REX::INFO("Starfield RT: PROBE     inst {} type {} material {} mode {} tex {} threshold {:.3f} uv ({:.3f}, {:.3f}) alpha {:.3f} at {:.1f} m -> {}",
                    u(c), u(c + 4) & 0xFF, (u(c + 4) >> 8) & 0xFFFF, u(c + 4) >> 24, u(c + 8), f(c + 12), f(c + 16), f(c + 20), f(c + 24), t,
                    (last & 1u) ? "SOLID (blocks)" : "see-through");
            }
        }

        void LogProbe(const std::uint8_t* d)
        {
            auto u = [d](std::size_t o) { std::uint32_t v; std::memcpy(&v, d + o, 4); return v; };
            auto f = [d](std::size_t o) { float v; std::memcpy(&v, d + o, 4); return v; };
            if (u(0) != 0x52545052u) {
                REX::WARN("Starfield RT: probe result not written");
                return;
            }
            REX::INFO("Starfield RT: PROBE at screen centre: depth {:g} -> surface {:.2f} m away at world ({:.1f}, {:.1f}, {:.1f})",
                f(24), f(12), f(52), f(56), f(60));
            {
                const float tx = f(292), ty = f(296), tz = f(300);
                REX::INFO("Starfield RT: PROBE sun direction (to sun) ({:.3f}, {:.3f}, {:.3f}), elevation {:.1f} deg, azimuth {:.1f} deg | camera row16 ({:.3f}, {:.3f}, {:.3f})",
                    tx, ty, tz, std::asin(std::clamp(tz, -1.0f, 1.0f)) * 57.2958f, std::atan2(ty, tx) * 57.2958f, f(304), f(308), f(312));
            }
            if (u(4)) {
                const float t = f(8), dist = f(12);
                REX::INFO("Starfield RT: PROBE primary ray HIT instance {} (mesh {}, triangle {}) at {:.2f} m -> {}",
                    u(16), u(28), u(20), t, std::abs(t - dist) < (std::max)(0.05f, dist * 0.02f) ? "matches the depth buffer" :
                    (t < dist ? "IN FRONT of the visible surface (something invisible?)" : "behind the visible surface (visible mesh missing?)"));
                REX::INFO("Starfield RT: PROBE   instance record: {}", Dwords(d + 64, 96));
                REX::INFO("Starfield RT: PROBE   LOD entries:     {}", Dwords(d + 160, 80));
                REX::INFO("Starfield RT: PROBE   mesh record:     {}", Dwords(d + 240, 52));
            } else {
                REX::INFO("Starfield RT: PROBE primary ray missed the TLAS (visible surface not in the TLAS)");
            }
            if (u(32)) {
                REX::INFO("Starfield RT: PROBE sun ray BLOCKED by instance {} (mesh {}, triangle {}) {:.2f} m from the surface, {} face, "
                          "{} tris, ~{:.1f} m across, placed at ({:.1f}, {:.1f}, {:.1f})",  // 0.5.58
                    u(40), u(48), u(44), f(36), u(316) ? "front" : "back", u(496 + 32) / 3,
                    f(496 + 48) * 32767.0f * ((u(416) & 0x40000000u) ? 2.0f : 1.0f) * f(320 + 44), f(320 + 32), f(320 + 36), f(320 + 40));
                REX::INFO("Starfield RT: PROBE   instance record: {}", Dwords(d + 320, 96));
                REX::INFO("Starfield RT: PROBE   LOD entries:     {}", Dwords(d + 416, 80));
                REX::INFO("Starfield RT: PROBE   mesh record:     {}", Dwords(d + 496, 52));
            } else {
                REX::INFO("Starfield RT: PROBE sun ray reached the sun (no RT shadow here)");
            }
            LogCandidates(d, "centre");
            if (u(552)) {
                REX::INFO("Starfield RT: PROBE   UV fetch debug: inst {} prim {} mesh {} firstIndex {} (vertex count {}, position handle {:#x}) "
                          "-> index window slot {} dword {:#010x} -> vertex indices {} {} {} | UV handle {:#x} -> raw UV words {:#010x} {:#010x} {:#010x}",
                    u(1440), u(1444), u(1448), u(1452), u(1492), u(1496), u(1484), u(1488), u(1456), u(1460), u(1464),
                    u(1468), u(1472), u(1476), u(1480));
            }
            if (u(4)) REX::INFO("Starfield RT: PROBE   primary hit material: {}", Dwords(d + 704, 368));
            if (u(32)) REX::INFO("Starfield RT: PROBE   sun blocker material: {}", Dwords(d + 1072, 368));
        }

        std::uint32_t Dw(const std::uint8_t* d, std::size_t o) { std::uint32_t v; std::memcpy(&v, d + o, 4); return v; }
        float         Fl(const std::uint8_t* d, std::size_t o) { float v; std::memcpy(&v, d + o, 4); return v; }

        // Groups the grid samples by instance: which instances are visible (primary ray matches the depth
        // buffer) and which only block sun rays, with each one's records, so a field that marks invisible
        // or non-shadow-casting proxies can be spotted.
        void LogProbeGrid(const std::uint8_t* d)
        {
            struct Info { int visible = 0, blocks = 0, front = 0, back = 0; float minT = 1e30f, maxT = 0; const std::uint8_t* rec = nullptr; const std::uint8_t* mat = nullptr; };
            int alphaSeen = 0, alphaSolid = 0, loggedCandidates = 0;
            std::map<std::uint32_t, Info> by;
            int surfaces = 0, shadowed = 0, primaryMiss = 0, mismatch = 0, inFront = 0, behind = 0;
            for (UINT i = 1; i < kProbeSamples; ++i) {
                const std::uint8_t* s = d + i * kSampleBytes;
                if (Dw(s, 0) != 0x52545052u || Fl(s, 24) <= 0.0f) continue;  // not written / sky
                ++surfaces;
                const float dist = Fl(s, 12);
                if (Dw(s, 4)) {
                    const float t = Fl(s, 8);
                    if (std::abs(t - dist) < (std::max)(0.05f, dist * 0.02f)) {
                        auto& e = by[Dw(s, 16)];
                        ++e.visible;
                        if (!e.rec) { e.rec = s + 64; e.mat = s + 704; }
                    } else {
                        ++mismatch;
                        (t < dist ? inFront : behind)++;
                    }
                } else {
                    ++primaryMiss;
                }
                if (Dw(s, 32)) {
                    ++shadowed;
                    auto& e = by[Dw(s, 40)];
                    ++e.blocks;
                    (Dw(s, 316) ? e.front : e.back)++;
                    e.minT = (std::min)(e.minT, Fl(s, 36));
                    e.maxT = (std::max)(e.maxT, Fl(s, 36));
                    e.rec = s + 320;  // prefer the sun-ray copy (same records)
                    e.mat = s + 1072;
                }
                const std::uint32_t nc = (std::min)(Dw(s, 552), 4u);
                for (std::uint32_t c = 0; c < nc; ++c) {
                    ++alphaSeen;
                    alphaSolid += Dw(s, 560 + c * 32 + 28) & 1u;
                }
                if (nc && loggedCandidates < 8) {  // 0.1.26: details for the first 8 grid samples that met any
                    ++loggedCandidates;
                    LogCandidates(s, "grid");
                }
            }
            REX::INFO("Starfield RT: PROBE GRID {}x{}: {} surface samples, {} sun-blocked, {} primary misses, {} primary/depth mismatches ({} TLAS in front, {} TLAS behind), {} distinct instances",
                kGridX, kGridY, surfaces, shadowed, primaryMiss, mismatch, inFront, behind, by.size());
            REX::INFO("Starfield RT: PROBE GRID alpha test: {} alpha-tested surfaces met, {} counted as solid", alphaSeen, alphaSolid);
            // Blockers that are never visible first: those are the suspects.
            std::vector<std::pair<std::uint32_t, Info>> list(by.begin(), by.end());
            std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                const bool sa = a.second.blocks && !a.second.visible, sb = b.second.blocks && !b.second.visible;
                if (sa != sb) return sa;
                return a.second.blocks + a.second.visible > b.second.blocks + b.second.visible;
            });
            int n = 0;
            for (const auto& [inst, e] : list) {
                if (++n > 48) break;
                const std::uint8_t* r = e.rec;
                const float meshScale = Fl(r, 176 + 48), instScale = Fl(r, 44);
                const float extent = meshScale * 32767.0f * ((Dw(r, 176) & 0x40000000u) ? 2.0f : 1.0f) * instScale;
                REX::INFO("Starfield RT: PROBE   {} inst {} mesh {} type {} material {} | visible {} blocks {} (front {} back {}, {:.1f}-{:.1f} m) | {} tris, ~{:.1f} m, at ({:.1f}, {:.1f}, {:.1f})",
                    e.blocks && !e.visible ? "SUSPECT" : (e.visible ? "visible" : "-------"), inst, Dw(r, 96 + 4),
                    Dw(r, 96) & 0x1F, (Dw(r, 96) >> 13) & 0x7FFF,
                    e.visible, e.blocks, e.front, e.back, e.blocks ? e.minT : 0.0f, e.blocks ? e.maxT : 0.0f,
                    Dw(r, 176 + 32) / 3, extent, Fl(r, 32), Fl(r, 36), Fl(r, 40));
                REX::INFO("Starfield RT: PROBE     inst {} | lod {} | mesh {}", Dwords(r, 96), Dwords(r + 96, 20), Dwords(r + 176, 52));
                if (e.mat && ((Dw(r, 96) & 0x1F) == 0 || (Dw(r, 96) & 0x1F) == 3))
                    REX::INFO("Starfield RT: PROBE     material {}", Dwords(e.mat, (Dw(r, 96) & 0x1F) == 0 ? 288 : 368));
            }
        }

        void PollProbe(std::uint64_t frame)
        {
            if (!g_probePendingFrame || frame < g_probePendingFrame + 5) return;
            g_probePendingFrame = 0;
            void* p = nullptr;
            const D3D12_RANGE range{ 0, kProbeBytes };
            if (SUCCEEDED(g_probeReadback->Map(0, &range, &p)) && p) {
                LogProbe(static_cast<const std::uint8_t*>(p));
                LogProbeGrid(static_cast<const std::uint8_t*>(p));
                const D3D12_RANGE none{ 0, 0 };
                g_probeReadback->Unmap(0, &none);
            }
        }

        void RecordProbe(ID3D12Device* device, ID3D12GraphicsCommandList* list, const SunShadowInputs& in)
        {
            if (!g_probeRequested.exchange(false)) return;
            if (!in.instances || !in.lods || !in.meshes) {
                REX::WARN("Starfield RT: probe skipped: scene record buffers not resolved this frame");
                return;
            }
            std::call_once(g_probeOnce, [device] { g_probeReady = CreateProbe(device); });
            if (!g_probeReady || g_probePendingFrame) return;
            struct { UINT w, h, px, py; float biasBase, biasPerUnit; UINT gridX, gridY, materials, vegMaterials, params, pad; } c{
                in.width, in.height, in.width / 2, in.height / 2, g_settings.biasBase, g_settings.biasPerUnit, kGridX, kGridY,
                in.materials && in.indexPool ? in.materialCount : 0u, in.vegMaterials && in.indexPool ? in.vegMaterialCount : 0u,
                in.alphaParams ? in.alphaParamsCount : 0u, 0u };
            list->SetComputeRootSignature(g_probeSig.Get());
            list->SetPipelineState(g_probePso.Get());
            list->SetComputeRoot32BitConstants(0, 12, &c, 0);
            list->SetComputeRootConstantBufferView(1, in.perScene);
            list->SetComputeRootConstantBufferView(2, in.contactData);
            list->SetComputeRootShaderResourceView(3, in.tlas);
            list->SetComputeRootShaderResourceView(4, in.instances);
            list->SetComputeRootShaderResourceView(5, in.lods);
            list->SetComputeRootShaderResourceView(6, in.meshes);
            list->SetComputeRootUnorderedAccessView(7, g_probeOut->GetGPUVirtualAddress());
            list->SetComputeRootDescriptorTable(8, in.blurTable);
            list->SetComputeRootShaderResourceView(9, in.materials ? in.materials : in.lods);
            list->SetComputeRootShaderResourceView(10, in.vegMaterials ? in.vegMaterials : in.lods);
            list->SetComputeRootShaderResourceView(11, in.alphaParams ? in.alphaParams : in.lods);
            list->SetComputeRootShaderResourceView(12, in.indexPool ? in.indexPool : in.lods);
            list->Dispatch((kProbeSamples + 63) / 64, 1, 1);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { g_probeOut.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
            list->ResourceBarrier(1, &b);
            list->CopyBufferRegion(g_probeReadback.Get(), 0, g_probeOut.Get(), 0, kProbeBytes);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            list->ResourceBarrier(1, &b);
            g_probePendingFrame = in.frame;
            REX::INFO("Starfield RT: probe recorded (frame {}), result in ~5 frames", in.frame);
        }
    }

    void RequestProbe() { g_probeRequested = true; }

    void SetSunShadowSettings(const SunShadowSettings& s)
    {
        g_settings = s;
        g_softLive = s.soft;
        g_liveMode = -1;  // 0.2.36: a quality change from Gameplay Options wins over the F11 toggle
    }

    bool SoftShadowsOn() { return g_softLive.load(); }

    namespace { std::atomic<int> g_beforeLitTest{ -1 }; }

    bool ToggleLitTest()
    {
        const int before = g_beforeLitTest.exchange(-1);
        if (before >= 0) {  // leaving the lit test: back to the previous mode
            g_liveMode = before;
            return false;
        }
        g_beforeLitTest = CurrentSunMode();
        g_liveMode = 6;
        return true;
    }

    bool ToggleSoftShadows()
    {
        const bool next = !g_softLive.load();
        g_softLive = next;
        return next;
    }

    int CurrentSunMode() { return g_liveMode.load() < 0 ? g_settings.mode : g_liveMode.load(); }

    void SetSunShadowsLive(int mode) { g_liveMode = mode; }  // 0.5.28

    int ToggleSunShadows()
    {
        const int configured = g_settings.mode != 0 ? g_settings.mode : 1;
        const int current = g_liveMode.load() < 0 ? g_settings.mode : g_liveMode.load();
        const int next = current == 0 ? configured : 0;
        g_liveMode = next;
        return next;
    }

    const char* RecordSunShadows(ID3D12Device* device, ID3D12GraphicsCommandList* list, const SunShadowInputs& in)
    {
        PollProbe(in.frame);
        const int mode = g_liveMode.load() < 0 ? g_settings.mode : g_liveMode.load();
        if (!in.tlas) return "skipped: no TLAS yet";
        if (!in.perScene || !in.contactData) {
            // 0.2.61: in some interiors the shadow pass binds neither; the F8 probe still runs with the scene table's
            // PerSceneConstants (sun-ray fields are then meaningless). RT sun shadows stay off there, as before.
            if (in.probePerScene && in.blurTable.ptr && in.width && in.height) {
                SunShadowInputs p = in;
                p.perScene = in.perScene ? in.perScene : in.probePerScene;
                if (!p.contactData) p.contactData = p.perScene;
                RecordProbe(device, list, p);
            }
            return !in.perScene ? "skipped: PerSceneConstants not found" : "skipped: contact-shadow constants not found";
        }
        if (!in.blurTable.ptr || !in.width || !in.height) return "skipped: no blur bindings";
        if (mode == 0) {
            RecordProbe(device, list, in);  // the probe works with RT shadows off too
            return "off";
        }
        std::call_once(g_once, [device] { g_ready = CreatePipeline(device); });
        if (!g_ready) return "skipped: pipeline unavailable";

        const bool alpha = (in.materialCount || in.vegMaterialCount) && in.lods && in.meshes && in.indexPool;
        // 0.1.30: soft shadows need the per-pixel buffer (8 B per pixel of the dispatch extent).
        bool soft = g_softLive.load() && g_denoisePso && (mode == 1 || mode == 2);
        {  // allocated even with soft shadows off: every root parameter must be bound
            const std::uint64_t need = std::uint64_t(in.width) * in.height * 8;
            if (need > g_visBytes) {
                if (g_visBuffer) g_oldVisBuffers.push_back(std::move(g_visBuffer));
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = need;
                desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        nullptr, IID_PPV_ARGS(&g_visBuffer)))) {
                    g_visBytes = need;
                    REX::INFO("Starfield RT: soft shadow buffer {}x{} ({:.1f} MB)", in.width, in.height, need / 1048576.0);
                } else {
                    g_visBytes = 0;
                }
            }
        }
        if (!g_visBuffer) return "skipped: soft shadow buffer allocation failed";
        const float sunTan = std::tan((std::clamp)(g_settings.sunRadiusDeg, 0.0f, 10.0f) * 0.0174532925f);
        struct {
            UINT w, h, mode; float maxDist, biasBase, biasPerUnit; UINT alphaFlags, materialCount, vegCount, paramsCount;
            float sunTan; UINT rays, frame, softFlags;
        } c{
            in.width, in.height, static_cast<UINT>(mode), g_settings.maxDistance, g_settings.biasBase, g_settings.biasPerUnit,
            alpha ? 1u : 0u, alpha && in.materials ? in.materialCount : 0u, alpha && in.vegMaterials ? in.vegMaterialCount : 0u,
            in.alphaParams ? in.alphaParamsCount : 0u,
            sunTan, static_cast<UINT>((std::clamp)(g_settings.rays, 1, 16)), static_cast<UINT>(in.frame),
            (soft ? 1u : 0u) | (static_cast<UINT>((std::clamp)(g_settings.maxBlurPx, 1, 32)) << 8) |
                (static_cast<UINT>((std::clamp)(g_settings.softScale, 1, 4)) << 16) };
        const UINT traceScale = soft ? static_cast<UINT>((std::clamp)(g_settings.softScale, 1, 4)) : 1u;
        static_assert(sizeof(c) == 56);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &uav);  // blur writes (and the TLAS build) complete
        list->SetComputeRootSignature(g_rootSig.Get());
        list->SetPipelineState(g_pso.Get());
        list->SetComputeRoot32BitConstants(0, 14, &c, 0);
        list->SetComputeRootConstantBufferView(1, in.perScene);
        list->SetComputeRootConstantBufferView(2, in.contactData);
        list->SetComputeRootShaderResourceView(3, in.tlas);
        list->SetComputeRootDescriptorTable(4, in.blurTable);
        // Root SRVs must point at real memory even when unused; the TLAS stands in when the records are missing.
        list->SetComputeRootShaderResourceView(5, in.lods ? in.lods : in.tlas);
        list->SetComputeRootShaderResourceView(6, in.meshes ? in.meshes : in.tlas);
        list->SetComputeRootShaderResourceView(7, in.materials ? in.materials : in.tlas);
        list->SetComputeRootShaderResourceView(8, in.vegMaterials ? in.vegMaterials : in.tlas);
        list->SetComputeRootShaderResourceView(9, in.alphaParams ? in.alphaParams : in.tlas);
        list->SetComputeRootShaderResourceView(10, in.indexPool ? in.indexPool : in.tlas);
        list->SetComputeRootUnorderedAccessView(11, g_visBuffer->GetGPUVirtualAddress());
        {
            const UINT tw = (in.width + traceScale - 1) / traceScale, th = (in.height + traceScale - 1) / traceScale;
            list->Dispatch((tw + 7) / 8, (th + 7) / 8, 1);  // 0.2.2: soft shadows trace at 1/traceScale resolution
        }
        list->ResourceBarrier(1, &uav);
        timer::Stamp(list, timer::kAfterShadow);
        if (soft) {
            // 0.1.30 pass 2: penumbra-aware filter from the vis buffer into the engine mask.
            list->SetPipelineState(g_denoisePso.Get());
            list->Dispatch((in.width + 7) / 8, (in.height + 7) / 8, 1);
            list->ResourceBarrier(1, &uav);
        }
        timer::Stamp(list, timer::kAfterFilter);
        RecordProbe(device, list, in);
        return "applied";
    }
}
