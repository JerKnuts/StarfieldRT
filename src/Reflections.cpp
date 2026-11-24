// RT reflections (see Reflections.h, shaders/RtReflectDebug.hlsl).

#include <SFSE/SFSE.h>  // CommonLibSF must come before any Windows headers

#include "Reflections.h"
#include "DeviceHooks.h"  // 0.2.60: FindUploadBuffer (local-light copies)
#include "StreamlineHooks.h"  // 0.5.7: RR raw mode
#include "generated/RtReflectDebugCS.h"
#include "generated/RtReflectResolveCS.h"
#include "generated/RtGlassCS.h"  // 0.2.51 (G3): Glass entry of shaders/RtReflectDebug.hlsl
#include "generated/RtGiTraceCS.h"     // 0.3.0: GI entries of shaders/RtReflectDebug.hlsl
#include "generated/RtGiTemporalCS.h"
#include "generated/RtGiResolveCS.h"
#include "generated/RtGiCaptureCS.h"   // 0.3.3
#include "generated/RtReflTemporalCS.h"  // 0.3.4
#include "generated/RtDlCullCS.h"        // 0.4.0: RT direct lighting
#include "generated/RtDlShadeCS.h"
#include "generated/RtLightCopyCS.h"  // 0.4.3
#include "generated/RtRrGuidesCS.h"   // 0.5.0

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>

#include <wrl/client.h>
#include <DirectXPackedVector.h>  // 0.3.6: float -> half

using Microsoft::WRL::ComPtr;

namespace rt
{
    namespace
    {
        ReflectionSettings          g_settings;
        std::atomic<int>            g_mode{ 0 };
        std::mutex                  g_lock;           // recording can happen on different render threads
        ComPtr<ID3D12RootSignature> g_sig;
        ComPtr<ID3D12PipelineState> g_pso;
        ComPtr<ID3D12PipelineState> g_resolvePso;     // 0.2.29: glossy filter + composite
        ComPtr<ID3D12PipelineState> g_glassPso;       // 0.2.51 (G3): thin-glass reflections
        ComPtr<ID3D12PipelineState> g_giTracePso, g_giTemporalPso, g_giResolvePso;  // 0.3.0: RT diffuse GI
        ComPtr<ID3D12PipelineState> g_giCapturePso;   // 0.3.3: last frame's lit image for GI
        ComPtr<ID3D12PipelineState> g_reflTemporalPso;  // 0.3.4: temporal accumulation of RT reflections
        ComPtr<ID3D12PipelineState> g_dlCullPso, g_dlShadePso;  // 0.4.0: RT direct lighting from the game's lamps
        ComPtr<ID3D12PipelineState> g_lightCopyPso;  // 0.4.3: GPU copy of the game's lamp list
        // 0.5.0: DLSS Ray Reconstruction guide images (diffuse albedo, specular albedo, normal+roughness, hit distance)
        ComPtr<ID3D12PipelineState> g_rrPso;
        ComPtr<ID3D12Resource>      g_rrBuf;
        std::uint64_t               g_rrBufBytes = 0;
        ComPtr<ID3D12Resource>      g_rrTex[4];
        UINT                        g_rrW = 0, g_rrH = 0;
        std::atomic<bool>           g_rrReady{ false };
        constexpr DXGI_FORMAT       kRrFormats[4] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                      DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT };
        ComPtr<ID3D12Resource>      g_dlTiles, g_dlDiff, g_dlSpec;   // tile lamp lists; RT0 / RT1 working copies (UAV)
        std::uint64_t               g_dlTilesBytes = 0, g_dlBufBytes = 0;
        std::atomic<int>            g_dlMode{ 1 };
        // 0.5.34: when our RT lamp pass last ran (ms tick). Was a countdown of 3 lamp-list captures, which ran out within
        // one frame outdoors (the game reads its lamp list several times per frame there): the game's lamps came back for
        // part of the frame and the whole picture flickered between two brightness levels at Jemison.
        std::atomic<std::uint64_t>  g_dlAppliedMs{ 0 };
        ComPtr<ID3D12Resource>      g_countsUpload;    // 0.4.0: CPU snapshots of the game's lamp index ranges (256 B per slot)
        std::uint8_t*               g_countsMapped = nullptr;
        std::atomic<bool>           g_lightsZeroable{ false };  // 0.4.2: the game's lamp ranges are CPU-writable here
        ID3D12Resource*             g_lightSlotLast = nullptr;  // 0.4.3: slot holding this frame's copy
        std::uint64_t               g_reflFrame = 0;
        bool                        g_reflHaveHistory = false;
        UINT                        g_reflW = 0, g_reflH = 0, g_reflTs = 0;
        ComPtr<ID3D12Resource>      g_giPrev, g_giPrevColor;  // 8 B/pixel colour + distance (UAV / SRV), HDR copy (footprint)
        std::uint64_t               g_giPrevBytes = 0, g_giPrevColorBytes = 0;
        bool                        g_giPrevReadable = false;  // g_giPrev in NON_PIXEL_SHADER_RESOURCE (else UAV)
        bool                        g_giPrevValid = false;
        int                         g_giPrevAge = 1000;  // GI passes since the last capture
        UINT                        g_giPrevW = 0, g_giPrevH = 0;
        ComPtr<ID3D12Resource>      g_giIn, g_giOut, g_giBuf;  // game's indirect diffuse copy, GI output, GI scratch
        std::uint64_t               g_giBytes = 0, g_giBufBytes = 0;
        std::uint64_t               g_giFrame = 0;
        bool                        g_giHaveHistory = false;
        UINT                        g_giW = 0, g_giH = 0, g_giTs = 0;
        std::atomic<int>            g_giMode{ 1 };
        ComPtr<ID3D12Resource>      g_glassBuf;       // 0.2.51: copy of the HDR target, COPY_DEST between uses
        std::uint64_t               g_glassBytes = 0;
        ReflectionInputs            g_lastIn{};       // 0.2.51: this frame's reflection inputs, for the glass pass
        int                         g_lastInAge = 1000;  // 0.2.52: glass passes since the reflection pass last recorded
        std::atomic<std::uint64_t>  g_lightCb{ 0 }, g_lightCounts{ 0 };  // 0.2.58: the game's local lights (0.2.60: our copy)

        // 0.2.60: the game's light records live in a per-frame upload ring, so reading them by address later (another
        // thread's reflection pass, recorded before or after the lighting pass) sometimes read last frame's slot after the
        // ring had reused it: the lights flickered. Now the lighting pass's own command list copies them into one of our
        // slots right when it binds them; the GPU runs lighting -> copy -> reflections in order, so reflections always read
        // a complete copy. Slot = 64 KB light records + 256 B index ranges.
        std::atomic<bool>           g_glassDumpRequested{ false };  // 0.2.62
        ComPtr<ID3D12Resource>      g_glassDumpReadback;
        int                         g_glassDumpPending = 0;  // glass passes to wait before reading (0 = none)
        constexpr UINT              kGlassDumpBytes = 432;

        void LogGlassDump(const std::uint32_t* d)
        {
            {  // 0.3.6: on-screen colour at the crosshair, for calibrating material emission
                float c[4];
                std::memcpy(c, &d[104], 16);
                REX::INFO("Starfield RT: [G3] F8 screen colour at the crosshair: HDR ({:.3f}, {:.3f}, {:.3f}), exposure {:.5f} -> radiance ({:.2f}, {:.2f}, {:.2f})",
                    c[0], c[1], c[2], c[3], c[0] / (std::max)(c[3], 1e-9f), c[1] / (std::max)(c[3], 1e-9f), c[2] / (std::max)(c[3], 1e-9f));
            }
            if (d[0] != 0x504D4447u) {
                REX::INFO("Starfield RT: [G3] F8 dump: the glass pass did not reach the screen-centre pixel "
                          "(not covered by OIT glass there, or the ray had no room)");
                return;
            }
            if (d[2] == 0xFFFFFFFFu) {
                REX::INFO("Starfield RT: [G3] F8 dump: centre pixel covered by OIT glass = {}, camera ray hit nothing", d[1]);
                return;
            }
            float dist;
            std::memcpy(&dist, &d[4], 4);
            const std::uint32_t bucket = d[5];
            auto words = [&](int first, int count) {
                std::string s;
                for (int i = 0; i < count; ++i) s += std::format("{}{:08x}", i ? " " : "", d[first + i]);
                return s;
            };
            REX::INFO("Starfield RT: [G3] F8 dump: covered {}, instance {}, tagged glass {}, {:.2f} m, LOD dword {:#x} (type {}, material {}), "
                      "triangle {}, material flags +108 = {:#x}", d[1], d[2], d[3], dist, bucket, bucket & 0x1Fu, (bucket >> 13) & 0x7FFFu,
                d[6], d[32 + 27]);
            REX::INFO("Starfield RT: [G3]   instance record: {}", words(8, 24));
            REX::INFO("Starfield RT: [G3]   material entry:  {}", words(32, 72));
        }

        std::atomic<bool>           g_mainDumpRequested{ false };  // 0.2.65: same for the main reflection pass
        ComPtr<ID3D12Resource>      g_mainDumpReadback;
        int                         g_mainDumpPending = 0;

        // 0.5.40: every lamp near the centre surface, as our RT lamp pass sees it (see DumpLamps in RtReflectDebug.hlsl).
        void LogLampDump(const std::uint32_t* d)
        {
            auto f = [&](int i) { float v; std::memcpy(&v, &d[i], 4); return v; };
            if (d[0] != 0x504D414Cu) {
                REX::INFO("Starfield RT: [lamps] probe: not written");
                return;
            }
            REX::INFO("Starfield RT: [lamps] probe at the screen centre ({:.2f} m away, pixel light channels {:#x}): {} lamp(s) within 8 m",
                f(3), d[2], d[1]);
            static constexpr const char* kType[3] = { "point", "box", "spot" };
            for (std::uint32_t k = 0; k < d[1] && k < 40; ++k) {
                const std::uint32_t* e = d + 16 + k * 20;  // 0.5.41: 80 bytes per lamp
                auto g = [&](int i) { float v; std::memcpy(&v, &e[i], 4); return v; };
                const std::uint32_t type = (e[0] >> 16) & 3u;
                REX::INFO("Starfield RT: [lamps]   #{} {} flags {:#x} ch {:#x} | dist {:.2f} att {:.4g} N.L {:.2f} lum {:.4g} cos(axis) {:.2f} | "
                          "k {:.4g} near {:.2f} p {:.2f}/{:.2f} r4 {:.3f}/{:.3f} coneEnd {:.3f} | {}{}",
                    e[0] & 0xFFFFu, kType[type < 3 ? type : 0], e[1], e[2], g(10), g(11), g(12), g(13), g(14), g(3), g(4), g(5), g(6),
                    g(7), g(8), g(9), (static_cast<int>(g(15)) & 1) ? "noInvSq " : "", (static_cast<int>(g(15)) & 2) ? "game-shadowed" : "");
                if (g(11) > 0.0f) {
                    if (g(16) < 0.0f) {
                        REX::INFO("Starfield RT: [lamps]       shadow ray ({:.2f} m): reaches the lamp", g(19));
                    } else {
                        REX::INFO("Starfield RT: [lamps]       shadow ray ({:.2f} m): BLOCKED at {:.2f} m by instance {} (LOD dword {:#x}: family {}, material {})",
                            g(19), g(16), e[17], e[18], e[18] & 0x1Fu, (e[18] >> 13) & 0x7FFFu);
                    }
                }
            }
        }

        // 0.5.47: 15 GI rays from the centre surface (DumpGiRays in RtReflectDebug.hlsl).
        void LogGiDump(const std::uint32_t* d)
        {
            auto f = [&](const std::uint32_t* p, int i) { float v; std::memcpy(&v, &p[i], 4); return v; };
            if (d[0] != 0x59524947u) {
                REX::INFO("Starfield RT: [gi] probe: not written");
                return;
            }
            int hits = 0, back = 0, skippedRays = 0;
            for (int i = 0; i < 15; ++i) {
                const std::uint32_t* e = d + 4 + i * 4;
                if (f(e, 0) >= 0.0f) { ++hits; if (!(e[1] & 1u)) ++back; }
                if (e[1] >> 4) ++skippedRays;
            }
            REX::INFO("Starfield RT: [gi] probe at the screen centre ({:.2f} m): G-buffer N.V {:.2f}{}; {} of 15 rays hit ({} on a back face), {} passed self-hits",
                f(d, 2), f(d, 1), d[3] ? " (facing away: flipped)" : "", hits, back, skippedRays);
            for (int i = 0; i < 15; ++i) {
                const std::uint32_t* e = d + 4 + i * 4;
                if (f(e, 0) < 0.0f) {
                    REX::INFO("Starfield RT: [gi]   ray {:2}: miss (dir.N {:.2f}, skipped {})", i, f(e, 3), e[1] >> 4);
                } else {
                    REX::INFO("Starfield RT: [gi]   ray {:2}: hit at {:.3f} m, {} face, hit group {}, instance {} (dir.N {:.2f}, skipped {})",
                        i, f(e, 0), (e[1] & 1u) ? "front" : "back", (e[1] >> 1) & 7u, e[2], f(e, 3), e[1] >> 4);
                }
            }
        }

        void LogMainDump(const std::uint32_t* d)
        {
            if (d[0] != 0x504D444Du) {
                REX::INFO("Starfield RT: [R] F8 dump: not written (reflection pass off or not in RT mode)");
                return;
            }
            auto f = [&](int i) { float v; std::memcpy(&v, &d[i], 4); return v; };
            REX::INFO("Starfield RT: [R] F8 dump (main reflections, screen centre): depth {:g}, G-buffer type word {:#x} (type {}), "
                      "AO {:.3f} rough {:.3f} metal {:.3f} mat.w {:.3f}, RT {} (keep game value {}), k {:.3f}, N.V {:.3f}",
                f(1), d[2], d[2] & 15u, f(3), f(4), f(5), f(6), (d[7] & 1u) ? "applied" : "NOT applied", (d[7] & 2u) ? "yes" : "no",
                f(8), f(9));
            REX::INFO("Starfield RT: [R]   weight ({:.3f}, {:.3f}, {:.3f}), game reflection ({:.4f}, {:.4f}, {:.4f}), albedo ({:.3f}, {:.3f}, {:.3f}), "
                      "material index .z {:.4f} (x255 = {:.0f})", f(10), f(11), f(12), f(13), f(14), f(15), f(16), f(17), f(18), f(19), f(19) * 255.0f);
            // 0.3.5: the object under the crosshair (for decoding emission)
            if (d[24] == 0xFFFFFFFFu) {
                REX::INFO("Starfield RT: [R]   crosshair ray: no hit");
                return;
            }
            auto words = [&](int first, int count) {
                std::string s;
                for (int i = 0; i < count; ++i) s += std::format("{}{:08x}", i ? " " : "", d[first + i]);
                return s;
            };
            REX::INFO("Starfield RT: [R]   crosshair ray: instance {} mesh {} at {:.2f} m, LOD dword {:#x} (family {}, material {}), texture set {}",
                d[24], d[28], f(26), d[25], d[25] & 0x1Fu, (d[25] >> 13) & 0x7FFFu, d[27] == 0xFFFFFFFFu ? std::string("n/a") : std::to_string(d[27]));
            if ((d[25] & 0x1Fu) == 0) {
                REX::INFO("Starfield RT: [R]   material entry: {}", words(32, 72));
                if (d[27] != 0xFFFFFFFFu) REX::INFO("Starfield RT: [R]   texture set:    {}", words(104, 10));
            } else {
                REX::INFO("Starfield RT: [R]   (family {} materials are not in the standard table; entry not dumped)", d[25] & 0x1Fu);
            }
        }

        constexpr UINT              kLightSlots = 6;  // 0.5.37: 4 -> 6 (a copy every frame now)
        constexpr UINT64            kLightSlotBytes = 65536 + 512;  // 0.4.3: + emptied ranges at 65760
        std::mutex                  g_lightLock;
        ComPtr<ID3D12Resource>      g_lightSlot[kLightSlots];
        bool                        g_lightSlotReadable[kLightSlots]{};  // false = still in its initial COPY_DEST state
        UINT                        g_lightNext = 0;
        D3D12_GPU_VIRTUAL_ADDRESS   g_lightLastSrc = 0;

        // 0.3.6: LocalShadowMax constant: shadow-ray count | emission model << 8 | half(emission scale) << 16.
        UINT PackLocalShadows()
        {
            const UINT rays = static_cast<UINT>((std::clamp)(g_settings.localShadowRays, 0, 16));
            const UINT hitRays = static_cast<UINT>((std::clamp)(g_settings.hitLampShadowRays, 0, 15));  // 0.4.5
            const float scale = (std::clamp)(g_settings.emissiveScale, 0.0f, 60000.0f);
            return rays | (g_settings.emissiveModel ? 0x100u : 0u) | (hitRays << 9) |
                   (static_cast<UINT>(DirectX::PackedVector::XMConvertFloatToHalf(scale)) << 16);
        }

        // 0.2.58: binds the local lights (root 17 CBV b3, root 18 SRV t11 space1); returns Flags bit 4 if they're usable.
        UINT BindLocalLights(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS fallbackCb, D3D12_GPU_VIRTUAL_ADDRESS fallbackSrv)
        {
            const auto cb = g_lightCb.load(), counts = g_lightCounts.load();
            const bool ok = g_settings.localLights && cb && counts;
            list->SetComputeRootConstantBufferView(17, ok ? cb : fallbackCb);
            list->SetComputeRootShaderResourceView(18, ok ? counts : fallbackSrv);
            return ok ? 16u : 0u;
        }
        ComPtr<ID3D12Resource>      g_rad;            // 0.2.29: per-pixel glossy samples, 16 B/pixel, UNORDERED_ACCESS
        std::uint64_t               g_radBytes = 0;
        std::once_flag              g_once;
        bool                        g_ready = false;
        ComPtr<ID3D12Resource>      g_out;            // R11G11B10 footprint, UNORDERED_ACCESS between uses
        ComPtr<ID3D12Resource>      g_in;             // 0.2.27: copy of the game's specular, COPY_DEST between uses
        std::uint64_t               g_bufBytes = 0;
        std::vector<ComPtr<ID3D12Resource>> g_old;    // kept alive after a resize (may still be in flight)
        // 0.5.7: RR raw mode. While DLSS Ray Reconstruction runs, reflections and GI hand it one fresh sample per pixel:
        // no temporal accumulation, no spatial blur (RR is trained on raw path-traced noise and does both itself).
        std::atomic<bool> g_rrRaw{ false };  // 0.5.10: off by default (flickers in game)
        bool RawNow() { return g_rrRaw.load() && RayReconstructionRunning(); }
        void NoteRaw(bool raw)
        {
            static int last = -1;
            if (static_cast<int>(raw) != last) {
                last = raw;
                REX::INFO("Starfield RT: [RR] raw mode {}", raw ? "on: reflections and GI unfiltered, 1 sample per pixel (RR denoises)"
                                                                 : "off: our own temporal + spatial filters");
            }
        }

        // 0.2.27: GPU time of the reflection pass (own timestamp ring: it runs after the main profiler's frame ends).
        constexpr UINT              kTimerSlots = 8;
        ComPtr<ID3D12QueryHeap>     g_queries;
        ComPtr<ID3D12Resource>      g_readback;
        const std::uint64_t*        g_stamps = nullptr;
        double                      g_ticksPerMs = 0.0;
        std::uint64_t               g_recorded = 0;
        bool                        g_slotUsed[kTimerSlots]{};
        double                      g_gpuSum = 0.0;
        std::uint32_t               g_gpuN = 0;

        // The order F2 steps through: off, RT reflections, then the debug views.
        constexpr int kCycle[] = { 0, 5, 1, 2, 3, 4, 6, 7 };  // 0.2.45: + 6 coverage; 0.2.46: + 7 material type

        bool CreatePipeline(ID3D12Device* device)
        {
            D3D12_DESCRIPTOR_RANGE1 range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 14, 0, 4,
                D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0 };  // t0..t13 space4
            D3D12_ROOT_PARAMETER1 p[20]{};
            p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants = { 1, 0, 27 };
            p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[1].Descriptor = { 0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[2].Descriptor = { 0, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };  // TLAS
            p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[3].DescriptorTable = { 1, &range };
            p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[4].Descriptor = { 0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            // 0.2.26 (R3): scene records t1..t7 space1 and the contact-shadow constants b2.
            for (UINT i = 0; i < 7; ++i) {
                p[5 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                p[5 + i].Descriptor = { 1 + i, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            }
            p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[12].Descriptor = { 2, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // 0.2.27: t8 space1, the game's specular
            p[13].Descriptor = { 8, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            p[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // 0.2.29: u1, glossy samples
            p[14].Descriptor = { 1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            for (UINT i = 0; i < 2; ++i) {  // 0.2.49: t9 vegetation materials, t10 alpha params (space1)
                p[15 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                p[15 + i].Descriptor = { 9 + i, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            }
            p[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // 0.2.58: b3, the game's local lights
            p[17].Descriptor = { 3, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            p[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // 0.2.58: t11 space1, their index ranges
            p[18].Descriptor = { 11, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            p[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // 0.3.3: t12 space1, last frame's lit image (GI)
            p[19].Descriptor = { 12, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
            for (auto& q : p) q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_STATIC_SAMPLER_DESC samplers[2]{};
            for (UINT i = 0; i < 2; ++i) {
                auto& s = samplers[i];
                s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                s.AddressU = s.AddressV = s.AddressW = i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                s.MaxLOD = D3D12_FLOAT32_MAX;
                s.ShaderRegister = i;
                s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            }
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
            desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            desc.Desc_1_1 = { 20, p, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED };
            ComPtr<ID3DBlob> blob, error;
            if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
                FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_sig)))) {
                REX::WARN("Starfield RT: reflection root signature failed: {}", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_sig.Get();
            pd.CS = { g_RtReflectDebugCS, sizeof(g_RtReflectDebugCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) {
                REX::WARN("Starfield RT: reflection pipeline failed");
                return false;
            }
            pd.CS = { g_RtReflectResolveCS, sizeof(g_RtReflectResolveCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_resolvePso)))) {
                REX::WARN("Starfield RT: reflection resolve pipeline failed");
                return false;
            }
            pd.CS = { g_RtGlassCS, sizeof(g_RtGlassCS) };
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_glassPso)))) {
                REX::WARN("Starfield RT: glass reflection pipeline failed (thin glass stays without RT reflections)");
                g_glassPso.Reset();
            }
            {  // 0.3.0: GI (optional)
                bool ok = true;
                pd.CS = { g_RtGiTraceCS, sizeof(g_RtGiTraceCS) };
                ok = ok && SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_giTracePso)));
                pd.CS = { g_RtGiTemporalCS, sizeof(g_RtGiTemporalCS) };
                ok = ok && SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_giTemporalPso)));
                pd.CS = { g_RtGiResolveCS, sizeof(g_RtGiResolveCS) };
                ok = ok && SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_giResolvePso)));
                pd.CS = { g_RtGiCaptureCS, sizeof(g_RtGiCaptureCS) };
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_giCapturePso)))) g_giCapturePso.Reset();
                pd.CS = { g_RtReflTemporalCS, sizeof(g_RtReflTemporalCS) };
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_reflTemporalPso)))) g_reflTemporalPso.Reset();
                pd.CS = { g_RtDlCullCS, sizeof(g_RtDlCullCS) };
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_dlCullPso)))) g_dlCullPso.Reset();
                pd.CS = { g_RtDlShadeCS, sizeof(g_RtDlShadeCS) };
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_dlShadePso)))) g_dlShadePso.Reset();
                pd.CS = { g_RtLightCopyCS, sizeof(g_RtLightCopyCS) };
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_lightCopyPso)))) g_lightCopyPso.Reset();
                pd.CS = { g_RtRrGuidesCS, sizeof(g_RtRrGuidesCS) };
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_rrPso)))) g_rrPso.Reset();
                if (!ok) {
                    REX::WARN("Starfield RT: GI pipeline failed (no RT GI)");
                    g_giTracePso.Reset();
                    g_giTemporalPso.Reset();
                    g_giResolvePso.Reset();
                }
            }
            // Timer (optional: reflections work without it).
            ComPtr<ID3D12CommandQueue> queue;
            D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
            UINT64 freq = 0;
            D3D12_QUERY_HEAP_DESC hd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimerSlots * 2, 0 };
            D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = kTimerSlots * 2 * sizeof(std::uint64_t);
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* mapped = nullptr;
            if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && SUCCEEDED(queue->GetTimestampFrequency(&freq)) && freq &&
                SUCCEEDED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&g_queries))) &&
                SUCCEEDED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&g_readback))) &&
                SUCCEEDED(g_readback->Map(0, nullptr, &mapped))) {
                g_stamps = static_cast<const std::uint64_t*>(mapped);
                g_ticksPerMs = static_cast<double>(freq) / 1000.0;
            } else {
                g_queries.Reset();
            }
            REX::INFO("Starfield RT: reflection pipeline created");
            return true;
        }

        D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
            return b;
        }

        bool CreateBuffer(ID3D12Device* device, std::uint64_t bytes, bool uav, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)));
        }

        // Reads the slot written kTimerSlots/2 recordings ago (long since finished on the GPU) and logs the average.
        void CollectTime(std::uint64_t index)
        {
            if (!g_stamps) return;
            const UINT s = static_cast<UINT>(index % kTimerSlots);
            if (g_slotUsed[s]) {
                const std::uint64_t a = g_stamps[s * 2], b = g_stamps[s * 2 + 1];
                const double ms = b > a ? static_cast<double>(b - a) / g_ticksPerMs : -1.0;
                if (ms >= 0.0 && ms < 1000.0) { g_gpuSum += ms; ++g_gpuN; }
                g_slotUsed[s] = false;
            }
            if (g_gpuN >= 600) {
                REX::INFO("Starfield RT: PERF reflections ({}, {} frames): {:.2f} ms GPU", ReflectionDebugName(g_mode.load()), g_gpuN,
                    g_gpuSum / g_gpuN);
                g_gpuSum = 0.0;
                g_gpuN = 0;
            }
        }
    }

    void SetReflectionSettings(const ReflectionSettings& s)
    {
        g_settings = s;
        g_giMode = (std::clamp)(s.giMode, 0, 2);  // 0.3.0
        g_dlMode = s.rtLamps ? 1 : 0;              // 0.4.0
        g_mode = s.startOn ? 5 : 0;  // 0.2.36: also called live from Gameplay Options
    }

    int CycleReflectionDebug()
    {
        const int cur = g_mode.load();
        int i = 0;
        while (i < static_cast<int>(std::size(kCycle)) && kCycle[i] != cur) ++i;
        const int next = kCycle[(i + 1) % std::size(kCycle)];
        g_mode = next;
        g_gpuSum = 0.0;
        g_gpuN = 0;
        return next;
    }

    int ReflectionDebugMode() { return g_mode.load(); }

    float AdjustReflectionBrightness(float factor)
    {
        std::lock_guard lock(g_lock);
        g_settings.probeAmbient = (std::clamp)(g_settings.probeAmbient * factor, 0.05f, 50.0f);
        g_settings.sunIntensity = (std::clamp)(g_settings.sunIntensity * factor, 0.01f, 50.0f);
        return g_settings.probeAmbient;
    }

    float AdjustReflectionGloss(float delta)
    {
        std::lock_guard lock(g_lock);
        g_settings.glossRoughScale = (std::clamp)(g_settings.glossRoughScale + delta, 0.2f, 1.0f);
        return g_settings.glossRoughScale;
    }

    float AdjustReflectionStrength(float delta)
    {
        std::lock_guard lock(g_lock);
        g_settings.glossStrength = (std::clamp)(g_settings.glossStrength + delta, 0.5f, 3.0f);
        return g_settings.glossStrength;
    }

    const char* ReflectionDebugName(int mode)
    {
        switch (mode) {
        case 1: return "debug: world normal";
        case 2: return "debug: mirror hit (instance colour, darker with distance; blue = miss)";
        case 3: return "debug: roughness";
        case 4: return "debug: RT reflection colour (albedo x sun/shadow + ambient at the hit)";
        case 5: return "RT reflections ON";
        case 7: return "debug: material type (0 black, 1 red, 2 green, 3 blue, 4 yellow, 5 cyan, 6 magenta, 7 white, 8 orange, 9 purple, 10 teal, 11 pink, 12 grey, 13 brown, 14 lime, 15 navy)";
        case 6: return "debug: coverage (green = RT replaces, red = left to the game: skin/hair type, yellow = left to the game, blue = material type)";
        default: return "off (game reflections)";
        }
    }

    const char* RecordReflectionDebug(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in)
    {
        const int mode = g_mode.load();
        if (mode == 0) return "off";
        if (!in.tlas) return "skipped: no TLAS";
        if (!in.perScene) return "skipped: PerSceneConstants not found";
        if (!in.gbufferTable.ptr) return "skipped: no G-buffer table";
        if (!in.target) return "skipped: specular target not found";
        const D3D12_RESOURCE_DESC td = in.target->GetDesc();
        if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.Format != DXGI_FORMAT_R11G11B10_FLOAT || td.SampleDesc.Count != 1) {
            return "skipped: specular target is not an R11G11B10 2D texture";
        }
        std::lock_guard lock(g_lock);
        std::call_once(g_once, [device] { g_ready = CreatePipeline(device); });
        if (!g_ready) return "skipped: pipeline unavailable";

        const UINT width = static_cast<UINT>(td.Width), height = td.Height;
        const UINT pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const std::uint64_t need = std::uint64_t(pitch) * height;
        if (need > g_bufBytes) {
            if (g_out) g_old.push_back(std::move(g_out));
            if (g_in) g_old.push_back(std::move(g_in));
            if (!CreateBuffer(device, need, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_out) ||
                !CreateBuffer(device, need, false, D3D12_RESOURCE_STATE_COPY_DEST, g_in)) {
                g_out.Reset();
                g_in.Reset();
                g_bufBytes = 0;
                return "skipped: buffer allocation failed";
            }
            g_bufBytes = need;
            REX::INFO("Starfield RT: reflection buffers {}x{} (2 x {:.1f} MB)", width, height, need / 1048576.0);
        }
        // 0.3.4: the glossy-sample buffer also holds two history copies of the trace grid and last frame's camera
        // (trace samples at 0, history at cells*16 and cells*32, camera rows at cells*48).
        const bool raw = RawNow();  // 0.5.7
        NoteRaw(raw);
        const UINT rts = raw ? 1u : static_cast<UINT>((std::clamp)(g_settings.traceScale, 1, 2));
        const std::uint64_t cells = std::uint64_t((width + rts - 1) / rts) * ((height + rts - 1) / rts);
        const std::uint64_t radNeed = (std::max)(std::uint64_t(width) * height * 16, cells * 48 + 512);
        if (radNeed > g_radBytes) {
            g_reflHaveHistory = false;
            if (g_rad) g_old.push_back(std::move(g_rad));
            if (!CreateBuffer(device, radNeed, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_rad)) {
                g_radBytes = 0;
                return "skipped: glossy buffer allocation failed";
            }
            g_radBytes = radNeed;
            REX::INFO("Starfield RT: glossy reflection buffer {}x{} ({:.1f} MB)", width, height, radNeed / 1048576.0);
        }

        const std::uint64_t index = g_recorded++;
        CollectTime(index + kTimerSlots / 2);  // the slot recorded kTimerSlots/2 frames ago
        const UINT slot = static_cast<UINT>(index % kTimerSlots);
        if (g_queries) list->EndQuery(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);

        D3D12_TEXTURE_COPY_LOCATION tex{};
        tex.pResource = in.target;
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION foot{};
        foot.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        foot.PlacedFootprint = { 0, { DXGI_FORMAT_R11G11B10_FLOAT, width, height, 1, pitch } };

        // 0.2.27 (R4): the composite blends with the game's own value, so copy it out first.
        const bool composite = mode == 5;
        D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (composite) {
            const D3D12_RESOURCE_BARRIER b = Transition(in.target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
            foot.pResource = g_in.Get();
            list->CopyTextureRegion(&foot, 0, 0, 0, &tex, nullptr);
            targetState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        {
            const D3D12_RESOURCE_BARRIER b = Transition(g_in.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &b);
        }

        // 0.2.26 (R3): hit shading needs every record; without them hits are shaded flat grey.
        const bool records = in.instances && in.lods && in.meshes && in.tints && in.indexPool && in.materials && in.textureSets;
        struct {
            UINT w, h, mode, pitch; float maxDist, scale; UINT materialCount, flags; float sun, ambient;
            float fadeStart, fadeEnd, probeAmbient, minWeight; UINT frame; float blurPx; UINT traceScale;
            UINT smoothTypes; float glassRoughScale; UINT vegCount, alphaParamsCount;
            UINT glassDepth, glassMoments; float glassStrength, glassMinCoverage; float localScale; UINT localShadows;
        } c{ width, height, static_cast<UINT>(mode), pitch, g_settings.maxDistance, g_settings.debugScale, records ? in.materialCount : 0u,
             (in.contactData ? 1u : 0u) | (g_settings.replaceAll ? 2u : 0u) | (g_settings.hitShadows ? 0u : 4u) |
                 (g_settings.offscreenProbeFloor ? 1024u : 0u),  // 0.5.32
             g_settings.sunIntensity, g_settings.ambient,
             // 0.5.24: in replace-all mode the fade range is unused; it carries the gloss (roughness scale) and strength
             g_settings.replaceAll ? g_settings.glossRoughScale : g_settings.fadeStart, g_settings.replaceAll ? g_settings.glossStrength : g_settings.fadeEnd,
             g_settings.probeAmbient, g_settings.replaceAll ? g_settings.replaceMinWeight : g_settings.minWeight,
             static_cast<UINT>(index), raw ? 0.0f : g_settings.blurPx, rts,
             g_settings.smoothGlassTypes, (std::clamp)(g_settings.glassRoughScale, 0.0f, 1.0f),
             records && in.vegMaterials ? in.vegMaterialCount : 0u, records && in.alphaParams ? in.alphaParamsCount : 0u,
             0u, 0u, 0.0f, 0.0f, (std::max)(g_settings.localLightScale, 0.0f), PackLocalShadows() };
        static_assert(sizeof(c) == 108);
        // 0.3.4: temporal accumulation (glassDepth = last frame's history, glassMoments = this frame's history = what the
        // resolve filters, glassStrength = camera rows offset as raw bits).
        const bool temporal = composite && g_reflTemporalPso && g_settings.reflTemporal;
        if (width != g_reflW || height != g_reflH || rts != g_reflTs) {
            g_reflW = width;
            g_reflH = height;
            g_reflTs = rts;
            g_reflHaveHistory = false;
        }
        const UINT histA = static_cast<UINT>(cells * 16), histB = static_cast<UINT>(cells * 32), prevOff = static_cast<UINT>(cells * 48);
        if (temporal) {
            const bool odd = (g_reflFrame & 1) != 0;
            c.glassDepth = odd ? histB : histA;
            c.glassMoments = odd ? histA : histB;
            std::memcpy(&c.glassStrength, &prevOff, 4);
            if (g_reflHaveHistory && !raw) c.flags |= 64u;  // 0.5.7: raw = the pass still runs (camera rows) but blends nothing
        }
        // 0.4.4: last frame's lit image for on-screen reflection hits (needs this pass's camera rows from last frame).
        if (g_giMode.load() == 0) ++g_giPrevAge;  // GI normally ages the capture; without it, do it here
        const bool prevImage = temporal && g_reflHaveHistory && g_giPrevValid && g_giPrevReadable && g_giPrev &&
                               g_giPrevW == width && g_giPrevH == height && g_giPrevAge <= 2;
        if (prevImage) c.flags |= 256u;
        {  // 0.4.5: say whether on-screen reflection hits use last frame's image (and why not)
            static int lastState = -1;
            const int state = !temporal ? 1 : !g_reflHaveHistory ? 2 : !g_giPrevValid ? 3 : (g_giPrevW != width || g_giPrevH != height) ? 4
                            : g_giPrevAge > 2 ? 5 : prevImage ? 0 : 6;
            if (state != lastState) {
                lastState = state;
                static const char* const kWhy[] = { "yes", "no: temporal accumulation off", "no: no camera history yet",
                    "no: last frame's image not captured (check '[GI] last-frame image capture')", "no: image size differs",
                    "no: image too old", "no" };
                REX::INFO("Starfield RT: [R] on-screen reflection hits use last frame's image: {}", kWhy[state]);
            }
        }
        list->SetComputeRootSignature(g_sig.Get());
        list->SetPipelineState(g_pso.Get());
        c.flags |= BindLocalLights(list, in.perScene, in.tlas);  // 0.2.58
        // 0.2.65: F8 dump of the screen-centre pixel (resolve pass), read back a few passes later
        if (g_mainDumpPending > 0 && --g_mainDumpPending == 0 && g_mainDumpReadback) {
            void* m = nullptr;
            const D3D12_RANGE r{ 0, 4096 };
            if (SUCCEEDED(g_mainDumpReadback->Map(0, &r, &m)) && m) {
                LogLampDump(static_cast<const std::uint32_t*>(m));         // 0.5.40: first 3328 bytes
                LogGiDump(static_cast<const std::uint32_t*>(m) + 832);     // 0.5.47: GI rays at 3328
                LogMainDump(static_cast<const std::uint32_t*>(m) + 896);   // the old 512-byte dump, at the end
                const D3D12_RANGE none{ 0, 0 };
                g_mainDumpReadback->Unmap(0, &none);
            }
        }
        bool mainDump = false;
        if (composite && g_mainDumpPending == 0 && g_mainDumpRequested.exchange(false)) {
            if (!g_mainDumpReadback) {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 4096;  // 0.5.40: + lamp probe
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&g_mainDumpReadback));
            }
            if (g_mainDumpReadback) {
                mainDump = true;
                c.flags |= 32u;
            }
        }
        list->SetComputeRoot32BitConstants(0, 27, &c, 0);
        list->SetComputeRootConstantBufferView(1, in.perScene);
        list->SetComputeRootShaderResourceView(2, in.tlas);
        list->SetComputeRootDescriptorTable(3, in.gbufferTable);
        list->SetComputeRootUnorderedAccessView(4, g_out->GetGPUVirtualAddress());
        // Root SRVs/CBVs must point at real memory even when unused: the TLAS / PerScene stand in.
        const D3D12_GPU_VIRTUAL_ADDRESS srvs[7] = { in.instances, in.lods, in.meshes, in.tints, in.indexPool, in.materials, in.textureSets };
        for (UINT i = 0; i < 7; ++i) list->SetComputeRootShaderResourceView(5 + i, records ? srvs[i] : in.tlas);
        list->SetComputeRootConstantBufferView(12, in.contactData ? in.contactData : in.perScene);
        list->SetComputeRootShaderResourceView(13, g_in->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(14, g_rad->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(15, records && in.vegMaterials ? in.vegMaterials : in.tlas);  // 0.2.49
        list->SetComputeRootShaderResourceView(16, records && in.alphaParams ? in.alphaParams : in.tlas);
        list->SetComputeRootShaderResourceView(19, prevImage ? g_giPrev->GetGPUVirtualAddress() : in.tlas);  // 0.4.4
        if (composite) {  // 0.2.34: the trace pass runs over the trace grid (1 ray per traceScale^2 pixels)
            const UINT s = c.traceScale;
            list->Dispatch(((width + s - 1) / s + 7) / 8, ((height + s - 1) / s + 7) / 8, 1);
        } else {
            list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
        if (composite) {
            // 0.2.29: pass 2, filter the glossy samples and composite into the output.
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = g_rad.Get();
            list->ResourceBarrier(1, &uav);
            if (temporal) {  // 0.3.4
                const UINT s = c.traceScale;
                list->SetPipelineState(g_reflTemporalPso.Get());
                list->Dispatch(((width + s - 1) / s + 7) / 8, ((height + s - 1) / s + 7) / 8, 1);
                list->ResourceBarrier(1, &uav);
            }
            list->SetPipelineState(g_resolvePso.Get());
            list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            if (mainDump) {  // 0.2.65
                const D3D12_RESOURCE_BARRIER b = Transition(g_rad.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->ResourceBarrier(1, &b);
                list->CopyBufferRegion(g_mainDumpReadback.Get(), 0, g_rad.Get(), std::uint64_t(width) * height * 16 - 4096, 4096);
                const D3D12_RESOURCE_BARRIER b2 = Transition(g_rad.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list->ResourceBarrier(1, &b2);
                g_mainDumpPending = 6;
            }
            // 0.3.4: this frame's camera rows for next frame's reprojection.
            std::uint64_t psOff = 0;
            ID3D12Resource* ps = temporal ? FindUploadBuffer(in.perScene, psOff) : nullptr;
            if (ps && psOff + 272 <= ps->GetDesc().Width) {  // 0.4.7: rows 0..16 (was 0..8)
                const D3D12_RESOURCE_BARRIER b = Transition(g_rad.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                list->ResourceBarrier(1, &b);
                list->CopyBufferRegion(g_rad.Get(), prevOff, ps, psOff, 272);
                const D3D12_RESOURCE_BARRIER b2 = Transition(g_rad.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list->ResourceBarrier(1, &b2);
                g_reflHaveHistory = true;
            } else {
                g_reflHaveHistory = false;
            }
            ++g_reflFrame;
        }

        D3D12_RESOURCE_BARRIER before[3] = {
            Transition(g_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(in.target, targetState, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(g_in.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(3, before);
        foot.pResource = g_out.Get();
        list->CopyTextureRegion(&tex, 0, 0, 0, &foot, nullptr);
        D3D12_RESOURCE_BARRIER after[2] = {
            Transition(g_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(in.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        list->ResourceBarrier(2, after);
        if (g_queries) {
            list->EndQuery(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
            list->ResolveQueryData(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, g_readback.Get(), slot * 2 * sizeof(std::uint64_t));
            g_slotUsed[slot] = true;
        }
        if (composite) {  // 0.2.51: the thin-glass pass later this frame reuses these inputs
            g_lastIn = in;
            g_lastInAge = 0;
        }
        return "applied";
    }

    int GiMode() { return g_giMode.load(); }

    int CycleGi()
    {
        const int cur = g_giMode.load();
        const int next = cur == 1 ? 2 : (cur == 2 ? 0 : 1);
        g_giMode = next;
        return next;
    }

    const char* GiModeName(int mode)
    {
        switch (mode) {
        case 1: return "RT GI on";
        case 2: return "debug: RT GI light alone (grey surfaces)";
        default: return "off (game's own GI)";
        }
    }

    const char* RecordGi(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in, ID3D12Resource* target)
    {
        const int mode = g_giMode.load();
        if (mode == 0) return "off";
        if (!in.tlas || !in.perScene || !in.gbufferTable.ptr) return "skipped: missing inputs";
        if (!target) return "skipped: indirect diffuse target not found";
        const D3D12_RESOURCE_DESC td = target->GetDesc();
        if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.Format != DXGI_FORMAT_R11G11B10_FLOAT || td.SampleDesc.Count != 1) {
            return "skipped: indirect diffuse target is not an R11G11B10 2D texture";
        }
        std::lock_guard lock(g_lock);
        std::call_once(g_once, [device] { g_ready = CreatePipeline(device); });
        if (!g_ready || !g_giTracePso || !g_giTemporalPso || !g_giResolvePso) return "skipped: pipeline unavailable";

        const UINT width = static_cast<UINT>(td.Width), height = td.Height;
        const UINT pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const std::uint64_t need = std::uint64_t(pitch) * height;
        if (need > g_giBytes) {
            if (g_giOut) g_old.push_back(std::move(g_giOut));
            if (g_giIn) g_old.push_back(std::move(g_giIn));
            if (!CreateBuffer(device, need, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_giOut) ||
                !CreateBuffer(device, need, false, D3D12_RESOURCE_STATE_COPY_DEST, g_giIn)) {
                g_giOut.Reset();
                g_giIn.Reset();
                g_giBytes = 0;
                return "skipped: buffer allocation failed";
            }
            g_giBytes = need;
        }
        const bool raw = RawNow();  // 0.5.7
        const UINT ts = raw ? 1u : static_cast<UINT>((std::clamp)(g_settings.giTraceScale, 1, 2));
        const UINT tw = (width + ts - 1) / ts, th = (height + ts - 1) / ts;
        const std::uint64_t cellBytes = std::uint64_t(tw) * th * 16;
        const std::uint64_t bufNeed = cellBytes * 3 + 512;  // 0.4.7: 17 camera rows
        if (bufNeed > g_giBufBytes) {
            if (g_giBuf) g_old.push_back(std::move(g_giBuf));
            if (!CreateBuffer(device, bufNeed, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_giBuf)) {
                g_giBufBytes = 0;
                return "skipped: GI buffer allocation failed";
            }
            g_giBufBytes = bufNeed;
            g_giHaveHistory = false;
            REX::INFO("Starfield RT: [GI] buffers {}x{}, trace grid {}x{} ({:.1f} MB)", width, height, tw, th,
                (bufNeed + 2 * need) / 1048576.0);
        }
        if (width != g_giW || height != g_giH || ts != g_giTs) {
            g_giW = width;
            g_giH = height;
            g_giTs = ts;
            g_giHaveHistory = false;
        }
        const UINT traceOff = 0;
        const UINT histA = static_cast<UINT>(cellBytes), histB = static_cast<UINT>(cellBytes * 2);
        const UINT prevOff = static_cast<UINT>(cellBytes * 3);
        const bool odd = (g_giFrame & 1) != 0;
        const UINT histRead = odd ? histB : histA, histWrite = odd ? histA : histB;

        D3D12_TEXTURE_COPY_LOCATION tex{};
        tex.pResource = target;
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION foot{};
        foot.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        foot.PlacedFootprint = { 0, { DXGI_FORMAT_R11G11B10_FLOAT, width, height, 1, pitch } };
        {
            const D3D12_RESOURCE_BARRIER b = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
        }
        foot.pResource = g_giIn.Get();
        list->CopyTextureRegion(&foot, 0, 0, 0, &tex, nullptr);
        {
            const D3D12_RESOURCE_BARRIER b = Transition(g_giIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &b);
        }

        const bool records = in.instances && in.lods && in.meshes && in.tints && in.indexPool && in.materials && in.textureSets;
        struct {
            UINT w, h, mode, pitch; float maxDist, scale; UINT materialCount, flags; float sun, ambient;
            float fadeStart, fadeEnd, probeAmbient, minWeight; UINT frame; float blurPx; UINT traceScale;
            UINT smoothTypes; float glassRoughScale; UINT vegCount, alphaParamsCount;
            UINT glassDepth, glassMoments; float glassStrength, glassMinCoverage; float localScale; UINT localShadows;
        } c{ width, height, prevOff, pitch, g_settings.maxDistance, static_cast<float>((std::clamp)(g_settings.giRays, 1, 8)),
             records ? in.materialCount : 0u,
             (in.contactData ? 1u : 0u) | (g_settings.hitShadows ? 0u : 4u) | (g_giHaveHistory && !raw ? 64u : 0u) | (mode == 2 ? 128u : 0u),
             g_settings.sunIntensity, g_settings.ambient,
             (std::max)(g_settings.giRayLength, 1.0f), (std::max)(g_settings.giHistory, 1.0f), g_settings.probeAmbient, 0.0f,
             static_cast<UINT>(g_giFrame), raw ? 0.0f : (std::max)(g_settings.giRadius, 0.0f), ts,
             histWrite, (std::max)(g_settings.giScreenLight, 0.0f),
             records && in.vegMaterials ? in.vegMaterialCount : 0u, records && in.alphaParams ? in.alphaParamsCount : 0u,
             traceOff, histRead, (std::max)(g_settings.giStrength, 0.0f), (std::max)(g_settings.giBounce, 0.0f),
             (std::max)(g_settings.localLightScale, 0.0f), PackLocalShadows() };
        static_assert(sizeof(c) == 108);
        list->SetComputeRootSignature(g_sig.Get());
        c.flags |= BindLocalLights(list, in.perScene, in.tlas);
        list->SetComputeRoot32BitConstants(0, 27, &c, 0);
        list->SetComputeRootConstantBufferView(1, in.perScene);
        list->SetComputeRootShaderResourceView(2, in.tlas);
        list->SetComputeRootDescriptorTable(3, in.gbufferTable);
        list->SetComputeRootUnorderedAccessView(4, g_giOut->GetGPUVirtualAddress());
        const D3D12_GPU_VIRTUAL_ADDRESS srvs[7] = { in.instances, in.lods, in.meshes, in.tints, in.indexPool, in.materials, in.textureSets };
        for (UINT i = 0; i < 7; ++i) list->SetComputeRootShaderResourceView(5 + i, records ? srvs[i] : in.tlas);
        list->SetComputeRootConstantBufferView(12, in.contactData ? in.contactData : in.perScene);
        list->SetComputeRootShaderResourceView(13, g_giIn->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(14, g_giBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(15, records && in.vegMaterials ? in.vegMaterials : in.tlas);
        list->SetComputeRootShaderResourceView(16, records && in.alphaParams ? in.alphaParams : in.tlas);
        list->SetComputeRootShaderResourceView(19, in.tlas);  // 0.3.3: PrevFrame placeholder (GI rebinds it)

        // 0.3.3: last frame's lit image, if captured at this size.
        const bool prevOk = g_giPrevValid && ++g_giPrevAge <= 2 && g_giPrev && g_giPrevReadable && g_giPrevW == width && g_giPrevH == height &&
                            g_settings.giScreenLight > 0.0f;
        if (prevOk) {
            list->SetComputeRootShaderResourceView(19, g_giPrev->GetGPUVirtualAddress());
            c.flags |= 256u;
            list->SetComputeRoot32BitConstants(0, 27, &c, 0);
        }
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = g_giBuf.Get();
        list->SetPipelineState(g_giTracePso.Get());
        list->Dispatch((tw + 7) / 8, (th + 7) / 8, 1);
        list->ResourceBarrier(1, &uav);
        list->SetPipelineState(g_giTemporalPso.Get());
        list->Dispatch((tw + 7) / 8, (th + 7) / 8, 1);
        list->ResourceBarrier(1, &uav);
        list->SetPipelineState(g_giResolvePso.Get());
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        // Output -> the game's indirect diffuse target.
        D3D12_RESOURCE_BARRIER before[3] = {
            Transition(g_giOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(g_giIn.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(3, before);
        foot.pResource = g_giOut.Get();
        list->CopyTextureRegion(&tex, 0, 0, 0, &foot, nullptr);
        D3D12_RESOURCE_BARRIER after[2] = {
            Transition(g_giOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        list->ResourceBarrier(2, after);

        // This frame's camera (PerScene rows 0..8) for next frame's reprojection.
        std::uint64_t psOff = 0;
        ID3D12Resource* ps = FindUploadBuffer(in.perScene, psOff);
        if (ps && psOff + 272 <= ps->GetDesc().Width) {  // 0.4.7: rows 0..16 (was 0..8)
            const D3D12_RESOURCE_BARRIER b = Transition(g_giBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &b);
            list->CopyBufferRegion(g_giBuf.Get(), prevOff, ps, psOff, 272);
            const D3D12_RESOURCE_BARRIER b2 = Transition(g_giBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &b2);
            g_giHaveHistory = true;
        } else {
            g_giHaveHistory = false;
        }
        ++g_giFrame;
        return g_giHaveHistory ? "applied" : "applied (no history: PerScene not in a known upload buffer)";
    }

    const char* RecordGiCapture(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* target, std::uint32_t depthIndex,
        std::uint32_t momentIndex)
    {
        // 0.4.4: also captured for reflections alone (their on-screen hits use it).
        if ((g_giMode.load() == 0 || g_settings.giScreenLight <= 0.0f) && g_mode.load() != 5) return "off";
        if (!device || !list || !target) return "skipped: missing inputs";
        const D3D12_RESOURCE_DESC td = target->GetDesc();
        if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.Format != DXGI_FORMAT_R11G11B10_FLOAT || td.SampleDesc.Count != 1) {
            return "skipped: HDR target is not an R11G11B10 2D texture";
        }
        std::lock_guard lock(g_lock);
        if (!g_ready || !g_giCapturePso) return "skipped: pipeline unavailable";
        if (g_lastInAge > 8 || !g_lastIn.perScene || !g_lastIn.tlas) return "skipped: no recent reflection inputs";
        const ReflectionInputs& in = g_lastIn;
        const UINT width = static_cast<UINT>(td.Width), height = td.Height;
        const UINT pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const std::uint64_t colorNeed = std::uint64_t(pitch) * height, prevNeed = std::uint64_t(width) * height * 8;
        if (colorNeed > g_giPrevColorBytes) {
            if (g_giPrevColor) g_old.push_back(std::move(g_giPrevColor));
            if (!CreateBuffer(device, colorNeed, false, D3D12_RESOURCE_STATE_COPY_DEST, g_giPrevColor)) {
                g_giPrevColorBytes = 0;
                return "skipped: buffer allocation failed";
            }
            g_giPrevColorBytes = colorNeed;
        }
        if (prevNeed > g_giPrevBytes) {
            if (g_giPrev) g_old.push_back(std::move(g_giPrev));
            if (!CreateBuffer(device, prevNeed, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_giPrev)) {
                g_giPrevBytes = 0;
                g_giPrevValid = false;
                return "skipped: buffer allocation failed";
            }
            g_giPrevBytes = prevNeed;
            g_giPrevReadable = false;
            REX::INFO("Starfield RT: [GI] last-frame image buffers {}x{} ({:.1f} MB)", width, height, (prevNeed + colorNeed) / 1048576.0);
        }

        D3D12_TEXTURE_COPY_LOCATION tex{};
        tex.pResource = target;
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION foot{};
        foot.pResource = g_giPrevColor.Get();
        foot.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        foot.PlacedFootprint = { 0, { DXGI_FORMAT_R11G11B10_FLOAT, width, height, 1, pitch } };
        {
            const D3D12_RESOURCE_BARRIER b = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
        }
        list->CopyTextureRegion(&foot, 0, 0, 0, &tex, nullptr);
        {
            std::vector<D3D12_RESOURCE_BARRIER> b{
                Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(g_giPrevColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            if (g_giPrevReadable) b.push_back(Transition(g_giPrev.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            list->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
        }
        struct {
            UINT w, h, mode, pitch; float maxDist, scale; UINT materialCount, flags; float sun, ambient;
            float fadeStart, fadeEnd, probeAmbient, minWeight; UINT frame; float blurPx; UINT traceScale;
            UINT smoothTypes; float glassRoughScale; UINT vegCount, alphaParamsCount;
            UINT glassDepth, glassMoments; float glassStrength, glassMinCoverage; float localScale; UINT localShadows;
        } c{ width, height, 0u, pitch, 0.0f, 1.0f, 0u, 0u, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, 1u, 0u, 0.0f, 0u, 0u,
             depthIndex, momentIndex, 0.0f, 0.0f, 0.0f, 0u };
        static_assert(sizeof(c) == 108);
        list->SetComputeRootSignature(g_sig.Get());
        list->SetPipelineState(g_giCapturePso.Get());
        list->SetComputeRoot32BitConstants(0, 27, &c, 0);
        list->SetComputeRootConstantBufferView(1, in.perScene);
        list->SetComputeRootShaderResourceView(2, in.tlas);
        list->SetComputeRootDescriptorTable(3, in.gbufferTable);  // unused, must be valid
        list->SetComputeRootUnorderedAccessView(4, g_giPrev->GetGPUVirtualAddress());
        for (UINT i = 0; i < 7; ++i) list->SetComputeRootShaderResourceView(5 + i, in.tlas);
        list->SetComputeRootConstantBufferView(12, in.perScene);
        list->SetComputeRootShaderResourceView(13, g_giPrevColor->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(14, g_giPrev->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(15, in.tlas);
        list->SetComputeRootShaderResourceView(16, in.tlas);
        list->SetComputeRootConstantBufferView(17, in.perScene);
        list->SetComputeRootShaderResourceView(18, in.tlas);
        list->SetComputeRootShaderResourceView(19, in.tlas);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        {
            const D3D12_RESOURCE_BARRIER b[2] = {
                Transition(g_giPrev.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                Transition(g_giPrevColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            list->ResourceBarrier(2, b);
        }
        g_giPrevReadable = true;
        g_giPrevValid = true;
        g_giPrevAge = 0;
        g_giPrevW = width;
        g_giPrevH = height;
        return "applied";
    }

    ID3D12Resource* CaptureLocalLights(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS lightCb,
        D3D12_GPU_VIRTUAL_ADDRESS counts, D3D12_GPU_DESCRIPTOR_HANDLE table, bool& switchOff)
    {
        switchOff = false;
        if (!device || !list || !lightCb || !counts) return nullptr;
        std::call_once(g_once, [device] { g_ready = CreatePipeline(device); });
        if (!g_ready || !g_lightCopyPso) return nullptr;
        std::lock_guard lock(g_lightLock);
        // 0.5.37: copy on every capture. The game rotates its lamp buffers (A, B, B, A, B, B... in the 0.5.36 log), and a
        // capture from the same address as last time was skipped, so every third frame the RT lamps used last frame's copy
        // (camera-relative positions from the previous frame) and the picture flickered outdoors in that rhythm.
        const bool fresh = true;
        if (fresh) {
            // 0.4.3: the game's lamp buffers aren't CPU-reachable (0.4.2 log), so copy them with a shader, recorded on
            // the game's own list right here: the GPU runs this copy before the game's lighting and our later passes.
            const UINT i = g_lightNext;
            if (!g_lightSlot[i]) {
                if (!CreateBuffer(device, kLightSlotBytes, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_lightSlot[i])) return nullptr;
                g_lightSlotReadable[i] = false;
            }
            ID3D12Resource* slot = g_lightSlot[i].Get();
            constexpr auto kRead = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (g_lightSlotReadable[i]) {
                const D3D12_RESOURCE_BARRIER b = Transition(slot, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list->ResourceBarrier(1, &b);
            }
            list->SetComputeRootSignature(g_sig.Get());
            list->SetPipelineState(g_lightCopyPso.Get());
            // Every root parameter gets a real address (the shader reads only 17, 18 and writes 4).
            list->SetComputeRootConstantBufferView(1, lightCb);
            list->SetComputeRootShaderResourceView(2, counts);
            list->SetComputeRootDescriptorTable(3, table);
            list->SetComputeRootUnorderedAccessView(4, slot->GetGPUVirtualAddress());
            for (UINT k = 5; k <= 11; ++k) list->SetComputeRootShaderResourceView(k, counts);
            list->SetComputeRootConstantBufferView(12, lightCb);
            list->SetComputeRootShaderResourceView(13, counts);
            list->SetComputeRootUnorderedAccessView(14, slot->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(15, counts);
            list->SetComputeRootShaderResourceView(16, counts);
            list->SetComputeRootConstantBufferView(17, lightCb);
            list->SetComputeRootShaderResourceView(18, counts);
            list->SetComputeRootShaderResourceView(19, counts);
            list->Dispatch(16, 1, 1);
            {
                const D3D12_RESOURCE_BARRIER b = Transition(slot, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
                list->ResourceBarrier(1, &b);
            }
            g_lightSlotReadable[i] = true;
            g_lightNext = (i + 1) % kLightSlots;
            g_lightLastSrc = lightCb;
            const D3D12_GPU_VIRTUAL_ADDRESS va = slot->GetGPUVirtualAddress();
            g_lightCb = va;
            g_lightCounts = va + 65536;
            g_lightSlotLast = slot;
            g_lightsZeroable = true;

        }
        // Switch the game's lamps off (its table's ranges entry -> our emptied copy) while our RT lamp pass is running.
        switchOff = g_dlMode.load() != 0 && g_dlAppliedMs.load() != 0 && GetTickCount64() - g_dlAppliedMs.load() < 250;
        return g_lightSlotLast;
    }

    const char* RecordRrGuides(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in, UINT width, UINT height)
    {
        if (!device || !list || !in.tlas || !in.perScene || !in.gbufferTable.ptr || !width || !height) return "skipped: missing inputs";
        std::lock_guard lock(g_lock);
        std::call_once(g_once, [device] { g_ready = CreatePipeline(device); });
        if (!g_ready || !g_rrPso) return "skipped: pipeline unavailable";
        auto align = [](std::uint64_t v, std::uint64_t a) { return (v + a - 1) & ~(a - 1); };
        const UINT pitch4 = static_cast<UINT>(align(std::uint64_t(width) * 4, 256)), pitch8 = static_cast<UINT>(align(std::uint64_t(width) * 8, 256));
        const std::uint64_t size4 = align(std::uint64_t(pitch4) * height, 512), size8 = align(std::uint64_t(pitch8) * height, 512);
        const std::uint64_t off[4] = { 0, size4, size4 * 2, size4 * 2 + size8 };
        const std::uint64_t need = off[3] + size4;
        if (need > g_rrBufBytes) {
            if (g_rrBuf) g_old.push_back(std::move(g_rrBuf));
            if (!CreateBuffer(device, need, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_rrBuf)) {
                g_rrBufBytes = 0;
                return "skipped: buffer allocation failed";
            }
            g_rrBufBytes = need;
        }
        constexpr auto kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (width != g_rrW || height != g_rrH || !g_rrTex[0]) {
            g_rrReady = false;
            for (int i = 0; i < 4; ++i) {
                if (g_rrTex[i]) g_old.push_back(std::move(g_rrTex[i]));
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                d.Width = width;
                d.Height = height;
                d.DepthOrArraySize = 1;
                d.MipLevels = 1;
                d.Format = kRrFormats[i];
                d.SampleDesc.Count = 1;
                if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kRead, nullptr, IID_PPV_ARGS(&g_rrTex[i])))) {
                    return "skipped: guide texture allocation failed";
                }
            }
            g_rrTex[0]->SetName(L"StarfieldRT RR diffuse albedo");
            g_rrTex[1]->SetName(L"StarfieldRT RR specular albedo");
            g_rrTex[2]->SetName(L"StarfieldRT RR normal roughness");
            g_rrTex[3]->SetName(L"StarfieldRT RR specular hit distance");
            g_rrW = width;
            g_rrH = height;
            REX::INFO("Starfield RT: [RR] guide images {}x{} ({:.1f} MB working buffer)", width, height, need / 1048576.0);
        }

        const bool records = in.instances && in.lods && in.meshes && in.tints && in.indexPool && in.materials && in.textureSets;
        struct {
            UINT w, h, mode, pitch; float maxDist, scale; UINT materialCount, flags; float sun, ambient;
            float fadeStart, fadeEnd, probeAmbient, minWeight; UINT frame; float blurPx; UINT traceScale;
            UINT smoothTypes; float glassRoughScale; UINT vegCount, alphaParamsCount;
            UINT glassDepth, glassMoments; float glassStrength, glassMinCoverage; float localScale; UINT localShadows;
        } c{ width, height, static_cast<UINT>(off[3]), pitch4, 1000.0f, 1.0f, records ? in.materialCount : 0u, 0u, 0.0f, 0.0f,
             0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, 1u, static_cast<UINT>(off[2]), 0.0f, 0u, 0u,
             pitch8, static_cast<UINT>(off[1]), 0.0f, 0.0f, 0.0f, 0u };
        static_assert(sizeof(c) == 108);
        list->SetComputeRootSignature(g_sig.Get());
        list->SetPipelineState(g_rrPso.Get());
        list->SetComputeRoot32BitConstants(0, 27, &c, 0);
        list->SetComputeRootConstantBufferView(1, in.perScene);
        list->SetComputeRootShaderResourceView(2, in.tlas);
        list->SetComputeRootDescriptorTable(3, in.gbufferTable);
        list->SetComputeRootUnorderedAccessView(4, g_rrBuf->GetGPUVirtualAddress());
        const D3D12_GPU_VIRTUAL_ADDRESS srvs[7] = { in.instances, in.lods, in.meshes, in.tints, in.indexPool, in.materials, in.textureSets };
        for (UINT i = 0; i < 7; ++i) list->SetComputeRootShaderResourceView(5 + i, records ? srvs[i] : in.tlas);
        list->SetComputeRootConstantBufferView(12, in.contactData ? in.contactData : in.perScene);
        list->SetComputeRootShaderResourceView(13, in.tlas);
        list->SetComputeRootUnorderedAccessView(14, g_rrBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(15, in.tlas);
        list->SetComputeRootShaderResourceView(16, in.tlas);
        list->SetComputeRootConstantBufferView(17, in.perScene);
        list->SetComputeRootShaderResourceView(18, in.tlas);
        list->SetComputeRootShaderResourceView(19, in.tlas);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER b[5] = { Transition(g_rrBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE) };
        for (int i = 0; i < 4; ++i) b[1 + i] = Transition(g_rrTex[i].Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(5, b);
        for (int i = 0; i < 4; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = g_rrTex[i].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = g_rrBuf.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = { off[i], { kRrFormats[i], width, height, 1, i == 2 ? pitch8 : pitch4 } };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        for (int i = 0; i < 5; ++i) std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
        list->ResourceBarrier(5, b);
        g_rrReady = true;
        return "applied";
    }

    bool GetRrGuides(ID3D12Resource* out[4], std::uint32_t& width, std::uint32_t& height)
    {
        if (!g_rrReady.load()) return false;
        for (int i = 0; i < 4; ++i) out[i] = g_rrTex[i].Get();
        width = g_rrW;
        height = g_rrH;
        return true;
    }

    // 0.5.23: the game's copy of the lit image made just before transparencies (the scene-colour mip chain the glass
    // samples). Handed to DLSS Ray Reconstruction as kBufferTypeColorBeforeTransparency, so it can tell smoke and glass
    // from the surfaces behind them instead of treating the smoke as part of the floor's reflection.
    namespace
    {
        std::mutex             g_cbtLock;
        ComPtr<ID3D12Resource> g_cbtTex;
        std::atomic<bool>      g_cbtOn{ false };  // 0.5.26: off by default
    }

    void NoteColorBeforeTransparency(ID3D12Resource* tex)
    {
        if (!tex || !g_cbtOn.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(g_cbtLock);
        if (g_cbtTex.Get() != tex) {
            g_cbtTex = tex;  // AddRef: the game has it bound right now, so it is alive
            static int changes = 0;  // 0.5.26: the game alternates between several copies; log only the first few
            if (++changes <= 4) {
                const D3D12_RESOURCE_DESC d = tex->GetDesc();
                REX::INFO("Starfield RT: [RR] colour-before-transparency source {:p}: {}x{} format {} mips {}", static_cast<void*>(tex),
                    d.Width, d.Height, static_cast<int>(d.Format), d.MipLevels);
            }
        }
    }

    ComPtr<ID3D12Resource> GetColorBeforeTransparency()
    {
        if (!g_cbtOn.load(std::memory_order_relaxed)) return nullptr;
        std::lock_guard lock(g_cbtLock);
        return g_cbtTex;
    }

    void SetColorBeforeTransparency(bool on) { g_cbtOn = on; }
    bool ToggleColorBeforeTransparency() { return !g_cbtOn.exchange(!g_cbtOn.load()); }

    int DirectLightsMode() { return g_dlMode.load(); }

    void SetRrRaw(bool on) { g_rrRaw = on; }
    bool ToggleThinGlassFallback() { return g_settings.thinGlassFallback = !g_settings.thinGlassFallback; }  // 0.5.13
    bool ToggleRrRaw() { return !g_rrRaw.exchange(!g_rrRaw.load()); }

    // 0.5.28: baseline (everything off) setters.
    void SetReflectionModeRaw(int mode) { g_mode = mode; }
    void SetGiModeRaw(int mode) { g_giMode = (std::clamp)(mode, 0, 2); }
    void SetDirectLightsRaw(int mode)
    {
        g_dlMode = mode ? 1 : 0;
        if (!mode) g_dlAppliedMs = 0;  // the game's lamps come back on the next frame
    }
    void SetThinGlassMode(int mode)
    {
        std::lock_guard lock(g_lock);
        g_settings.thinGlass = (std::clamp)(mode, 0, 2);
    }
    void SetReflectionLook(float gloss, float strength)
    {
        std::lock_guard lock(g_lock);
        g_settings.glossRoughScale = (std::clamp)(gloss, 0.2f, 1.0f);
        g_settings.glossStrength = (std::clamp)(strength, 0.5f, 3.0f);
    }

    int ToggleDirectLights()
    {
        const int next = g_dlMode.load() ? 0 : 1;
        g_dlMode = next;
        if (!next) g_dlAppliedMs = 0;  // the game's lamps come back on the next frame
        return next;
    }

    const char* RecordDirectLights(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in,
        ID3D12Resource* rt0, ID3D12Resource* rt1)
    {
        if (g_dlMode.load() == 0) return "off";
        if (!in.tlas || !in.perScene || !in.gbufferTable.ptr || !rt0 || !rt1) return "skipped: missing inputs";
        const D3D12_RESOURCE_DESC d0 = rt0->GetDesc(), d1 = rt1->GetDesc();
        if (d0.Format != DXGI_FORMAT_R11G11B10_FLOAT || d1.Format != DXGI_FORMAT_R11G11B10_FLOAT || d0.Width != d1.Width ||
            d0.Height != d1.Height || d0.SampleDesc.Count != 1) {
            return "skipped: targets are not matching R11G11B10 textures";
        }
        std::lock_guard lock(g_lock);
        std::call_once(g_once, [device] { g_ready = CreatePipeline(device); });
        if (!g_ready || !g_dlCullPso || !g_dlShadePso) return "skipped: pipeline unavailable";
        if (!g_lightCb.load() || !g_lightCounts.load()) return "skipped: no lamp list captured yet";
        if (!g_lightsZeroable.load()) return "skipped: the game's lamp list isn't CPU-writable here (its lamps can't be switched off)";

        const UINT width = static_cast<UINT>(d0.Width), height = d0.Height;
        const UINT pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const UINT tilesX = (width + 15) / 16, tilesY = (height + 15) / 16;
        const std::uint64_t need = std::uint64_t(pitch) * height, tilesNeed = std::uint64_t(tilesX) * tilesY * 528;  // 0.5.18: near + far lists per tile
        if (need > g_dlBufBytes) {
            if (g_dlDiff) g_old.push_back(std::move(g_dlDiff));
            if (g_dlSpec) g_old.push_back(std::move(g_dlSpec));
            if (!CreateBuffer(device, need, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_dlDiff) ||
                !CreateBuffer(device, need, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_dlSpec)) {
                g_dlDiff.Reset();
                g_dlSpec.Reset();
                g_dlBufBytes = 0;
                return "skipped: buffer allocation failed";
            }
            g_dlBufBytes = need;
        }
        if (tilesNeed > g_dlTilesBytes) {
            if (g_dlTiles) g_old.push_back(std::move(g_dlTiles));
            if (!CreateBuffer(device, tilesNeed, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_dlTiles)) {
                g_dlTilesBytes = 0;
                return "skipped: buffer allocation failed";
            }
            g_dlTilesBytes = tilesNeed;
            REX::INFO("Starfield RT: [lamps] buffers {}x{}, {}x{} tiles", width, height, tilesX, tilesY);
        }

        D3D12_TEXTURE_COPY_LOCATION t0{}, t1{}, f0{}, f1{};
        t0.pResource = rt0; t0.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        t1.pResource = rt1; t1.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f0.pResource = g_dlDiff.Get(); f0.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        f0.PlacedFootprint = { 0, { DXGI_FORMAT_R11G11B10_FLOAT, width, height, 1, pitch } };
        f1 = f0;
        f1.pResource = g_dlSpec.Get();
        {
            const D3D12_RESOURCE_BARRIER b[4] = {
                Transition(rt0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(rt1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(g_dlDiff.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(g_dlSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            list->ResourceBarrier(4, b);
        }
        list->CopyTextureRegion(&f0, 0, 0, 0, &t0, nullptr);
        list->CopyTextureRegion(&f1, 0, 0, 0, &t1, nullptr);
        {
            const D3D12_RESOURCE_BARRIER b[2] = {
                Transition(g_dlDiff.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(g_dlSpec.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            list->ResourceBarrier(2, b);
        }

        const bool records = in.instances && in.lods && in.meshes && in.tints && in.indexPool && in.materials && in.textureSets;
        struct {
            UINT w, h, mode, pitch; float maxDist, scale; UINT materialCount, flags; float sun, ambient;
            float fadeStart, fadeEnd, probeAmbient, minWeight; UINT frame; float blurPx; UINT traceScale;
            UINT smoothTypes; float glassRoughScale; UINT vegCount, alphaParamsCount;
            UINT glassDepth, glassMoments; float glassStrength, glassMinCoverage; float localScale; UINT localShadows;
        } c{ width, height, 0u, pitch, g_settings.maxDistance, 1.0f, records ? in.materialCount : 0u, 0u, 0.0f, 0.0f,
             0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, 1u, 0u, 0.0f,
             records && in.vegMaterials ? in.vegMaterialCount : 0u, records && in.alphaParams ? in.alphaParamsCount : 0u,
             tilesX, 0u, (std::max)(g_settings.rtLampScale, 0.0f), 0.0f,
             (std::max)(g_settings.localLightScale, 0.0f), PackLocalShadows() };
        static_assert(sizeof(c) == 108);
        list->SetComputeRootSignature(g_sig.Get());
        c.flags |= BindLocalLights(list, in.perScene, in.tlas);
        if (!(c.flags & 16u)) return "skipped: lamp list not usable";
        list->SetComputeRoot32BitConstants(0, 27, &c, 0);
        list->SetComputeRootConstantBufferView(1, in.perScene);
        list->SetComputeRootShaderResourceView(2, in.tlas);
        list->SetComputeRootDescriptorTable(3, in.gbufferTable);
        const D3D12_GPU_VIRTUAL_ADDRESS srvs[7] = { in.instances, in.lods, in.meshes, in.tints, in.indexPool, in.materials, in.textureSets };
        for (UINT i = 0; i < 7; ++i) list->SetComputeRootShaderResourceView(5 + i, records ? srvs[i] : in.tlas);
        list->SetComputeRootConstantBufferView(12, in.contactData ? in.contactData : in.perScene);
        list->SetComputeRootUnorderedAccessView(14, g_dlSpec->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(15, records && in.vegMaterials ? in.vegMaterials : in.tlas);
        list->SetComputeRootShaderResourceView(16, records && in.alphaParams ? in.alphaParams : in.tlas);
        list->SetComputeRootShaderResourceView(19, in.tlas);

        // Pass 1: lamp lists per tile.
        list->SetComputeRootUnorderedAccessView(4, g_dlTiles->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(13, in.tlas);
        list->SetPipelineState(g_dlCullPso.Get());
        list->Dispatch(tilesX, tilesY, 1);
        {
            const D3D12_RESOURCE_BARRIER b = Transition(g_dlTiles.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &b);
        }
        // Pass 2: shade, adding into the RT0 / RT1 copies.
        list->SetComputeRootUnorderedAccessView(4, g_dlDiff->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(13, g_dlTiles->GetGPUVirtualAddress());
        list->SetPipelineState(g_dlShadePso.Get());
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        {
            const D3D12_RESOURCE_BARRIER b[5] = {
                Transition(g_dlTiles.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(g_dlDiff.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(g_dlSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(rt0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(rt1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            list->ResourceBarrier(5, b);
        }
        list->CopyTextureRegion(&t0, 0, 0, 0, &f0, nullptr);
        list->CopyTextureRegion(&t1, 0, 0, 0, &f1, nullptr);
        {
            const D3D12_RESOURCE_BARRIER b[4] = {
                Transition(g_dlDiff.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(g_dlSpec.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(rt0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(rt1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
            };
            list->ResourceBarrier(4, b);
        }
        g_dlAppliedMs = GetTickCount64();  // 0.5.34: the game's lamps stay off while this pass keeps running
        return "applied";
    }

    void RequestGlassDump()
    {
        g_glassDumpRequested = true;
        g_mainDumpRequested = true;  // 0.2.65
    }

    void ForgetReflectionInputs()
    {
        {
            std::lock_guard lock(g_lock);
            g_lastIn = {};
            g_lastInAge = 1000;
            g_giPrevValid = false;  // 0.3.3
            g_reflHaveHistory = false;  // 0.3.4
            g_dlAppliedMs = 0;  // 0.4.0
        }
        g_lightCb = 0;
        g_lightCounts = 0;
        std::lock_guard lock(g_lightLock);
        g_lightLastSrc = 0;
    }

    bool ToggleOffscreenProbeFloor()  // 0.5.32
    {
        std::lock_guard lock(g_lock);
        g_settings.offscreenProbeFloor = !g_settings.offscreenProbeFloor;
        return g_settings.offscreenProbeFloor;
    }

    bool ToggleLocalLights()
    {
        std::lock_guard lock(g_lock);
        g_settings.localLights = !g_settings.localLights;
        return g_settings.localLights;
    }

    int ToggleThinGlass()
    {
        std::lock_guard lock(g_lock);
        g_settings.thinGlass = g_settings.thinGlass == 1 ? 2 : (g_settings.thinGlass == 2 ? 0 : 1);
        return g_settings.thinGlass;
    }

    const char* RecordGlassReflections(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* target,
        std::uint32_t depthIndex, std::uint32_t momentIndex, D3D12_GPU_VIRTUAL_ADDRESS tlas)
    {
        std::lock_guard lock(g_lock);
        if (!g_settings.thinGlass) return "off";
        if (g_mode.load() != 5) return "skipped: RT reflections off";
        if (!g_ready || !g_glassPso || !g_in || !g_rad) return "skipped: pipeline unavailable";
        // 0.2.52: render passes are recorded on several threads, so the OIT composite is often recorded before this
        // frame's reflection pass. The scene records are persistent buffers, so inputs up to a few frames old are fine;
        // the TLAS is always this frame's.
        if (++g_lastInAge > 8) return "skipped: no recent reflection pass";
        ReflectionInputs in = g_lastIn;
        if (!tlas) return "skipped: no TLAS this frame";  // 0.2.59: never the previous pass's TLAS (may be freed)
        in.tlas = tlas;
        if (!device || !list || !target || !in.tlas || !in.perScene) return "skipped: missing inputs";
        const D3D12_RESOURCE_DESC td = target->GetDesc();
        if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.Format != DXGI_FORMAT_R11G11B10_FLOAT || td.SampleDesc.Count != 1) {
            return "skipped: HDR target is not an R11G11B10 2D texture";
        }
        const UINT width = static_cast<UINT>(td.Width), height = td.Height;
        const UINT pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const std::uint64_t need = std::uint64_t(pitch) * height;
        if (need > g_glassBytes) {
            if (g_glassBuf) g_old.push_back(std::move(g_glassBuf));
            if (!CreateBuffer(device, need, true, D3D12_RESOURCE_STATE_COPY_DEST, g_glassBuf)) {
                g_glassBytes = 0;
                return "skipped: buffer allocation failed";
            }
            g_glassBytes = need;
            REX::INFO("Starfield RT: [G3] glass reflection buffer {}x{} ({:.1f} MB)", width, height, need / 1048576.0);
        }

        D3D12_TEXTURE_COPY_LOCATION tex{};
        tex.pResource = target;
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION foot{};
        foot.pResource = g_glassBuf.Get();
        foot.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        foot.PlacedFootprint = { 0, { DXGI_FORMAT_R11G11B10_FLOAT, width, height, 1, pitch } };

        // HDR target -> buffer, add the glass reflections in place, buffer -> HDR target.
        {
            const D3D12_RESOURCE_BARRIER b = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
        }
        list->CopyTextureRegion(&foot, 0, 0, 0, &tex, nullptr);
        {
            const D3D12_RESOURCE_BARRIER b[2] = {
                Transition(g_glassBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            list->ResourceBarrier(2, b);
        }

        const bool records = in.instances && in.lods && in.meshes && in.tints && in.indexPool && in.materials && in.textureSets;
        struct {
            UINT w, h, mode, pitch; float maxDist, scale; UINT materialCount, flags; float sun, ambient;
            float fadeStart, fadeEnd, probeAmbient, minWeight; UINT frame; float blurPx; UINT traceScale;
            UINT smoothTypes; float glassRoughScale; UINT vegCount, alphaParamsCount;
            UINT glassDepth, glassMoments; float glassStrength, glassMinCoverage; float localScale; UINT localShadows;
        } c{ width, height, 8u, pitch, g_settings.maxDistance, 1.0f, records ? in.materialCount : 0u,
             (in.contactData ? 1u : 0u) | (g_settings.hitShadows ? 0u : 4u) | (g_settings.thinGlass == 2 ? 8u : 0u) |
             (g_settings.thinGlassFallback ? 0u : 512u),
             g_settings.sunIntensity, g_settings.ambient,
             0.0f, 0.0f, g_settings.probeAmbient, 0.0f, 0u, 0.0f, 1u, 0u, 0.0f,
             records && in.vegMaterials ? in.vegMaterialCount : 0u, records && in.alphaParams ? in.alphaParamsCount : 0u,
             depthIndex, momentIndex, (std::max)(g_settings.thinGlassStrength, 0.0f), g_settings.thinGlassMinCoverage,
             (std::max)(g_settings.localLightScale, 0.0f), PackLocalShadows() };
        static_assert(sizeof(c) == 108);
        list->SetComputeRootSignature(g_sig.Get());
        list->SetPipelineState(g_glassPso.Get());
        c.flags |= BindLocalLights(list, in.perScene, in.tlas);  // 0.2.58
        // 0.2.62: F8 dump (read back a few glass passes later)
        if (g_glassDumpPending > 0 && --g_glassDumpPending == 0 && g_glassDumpReadback) {
            void* m = nullptr;
            const D3D12_RANGE r{ 0, kGlassDumpBytes };
            if (SUCCEEDED(g_glassDumpReadback->Map(0, &r, &m)) && m) {
                LogGlassDump(static_cast<const std::uint32_t*>(m));
                const D3D12_RANGE none{ 0, 0 };
                g_glassDumpReadback->Unmap(0, &none);
            }
        }
        bool dumpNow = false;
        if (g_glassDumpPending == 0 && g_glassDumpRequested.exchange(false)) {
            if (!g_glassDumpReadback) {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = kGlassDumpBytes;
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&g_glassDumpReadback));
            }
            if (g_glassDumpReadback) {
                dumpNow = true;
                c.flags |= 32u;
            }
        }
        list->SetComputeRoot32BitConstants(0, 27, &c, 0);
        list->SetComputeRootConstantBufferView(1, in.perScene);
        list->SetComputeRootShaderResourceView(2, in.tlas);
        list->SetComputeRootDescriptorTable(3, in.gbufferTable);  // unused by the glass pass, but must be valid
        list->SetComputeRootUnorderedAccessView(4, g_glassBuf->GetGPUVirtualAddress());
        const D3D12_GPU_VIRTUAL_ADDRESS srvs[7] = { in.instances, in.lods, in.meshes, in.tints, in.indexPool, in.materials, in.textureSets };
        for (UINT i = 0; i < 7; ++i) list->SetComputeRootShaderResourceView(5 + i, records ? srvs[i] : in.tlas);
        list->SetComputeRootConstantBufferView(12, in.contactData ? in.contactData : in.perScene);
        list->SetComputeRootShaderResourceView(13, g_in->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(14, g_rad->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(15, records && in.vegMaterials ? in.vegMaterials : in.tlas);
        list->SetComputeRootShaderResourceView(16, records && in.alphaParams ? in.alphaParams : in.tlas);
        list->SetComputeRootShaderResourceView(19, in.tlas);  // 0.3.3: PrevFrame placeholder (GI rebinds it)
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        if (dumpNow) {  // 0.2.62: copy the dump out of the glossy-sample buffer (it is rewritten every frame anyway)
            const D3D12_RESOURCE_BARRIER b = Transition(g_rad.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
            list->CopyBufferRegion(g_glassDumpReadback.Get(), 0, g_rad.Get(), 0, kGlassDumpBytes);
            const D3D12_RESOURCE_BARRIER b2 = Transition(g_rad.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &b2);
            g_glassDumpPending = 6;
        }
        {
            const D3D12_RESOURCE_BARRIER b = Transition(g_glassBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
        }
        list->CopyTextureRegion(&tex, 0, 0, 0, &foot, nullptr);
        {
            const D3D12_RESOURCE_BARRIER b[2] = {
                Transition(g_glassBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
            };
            list->ResourceBarrier(2, b);
        }
        return "applied";
    }
}
