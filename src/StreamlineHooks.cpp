// 0.4.11: Streamline logging hooks (step 1 of DLSS Ray Reconstruction, see RT_FINDINGS.md "DLSS Ray Reconstruction
// feasibility"). Patches Starfield.exe's import table entries for sl.interposer.dll and logs what the game asks of
// Streamline: features loaded at slInit, the buffers it tags for DLSS (type, size, format, state, lifecycle), the DLSS
// options and the per-frame constants. Every hook calls the original unchanged.

#include <SFSE/SFSE.h>  // CommonLibSF must come before any Windows headers

#include "StreamlineHooks.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>  // 0.5.0: Ray Reconstruction

#include <vector>

#include "Reflections.h"  // 0.5.0: RR guide images

namespace rt
{
    namespace
    {
        using PFn_slInit = sl::Result (*)(const sl::Preferences&, uint64_t);
        using PFn_slSetTag = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
        using PFn_slEvaluateFeature = sl::Result (*)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);
        using PFn_slSetConstants = sl::Result (*)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
        using PFn_slGetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
        using PFn_slDLSSSetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSOptions&);

        PFn_slInit                g_slInit = nullptr;
        PFn_slSetTag              g_slSetTag = nullptr;
        PFn_slEvaluateFeature     g_slEvaluateFeature = nullptr;
        PFn_slSetConstants        g_slSetConstants = nullptr;
        PFn_slGetFeatureFunction  g_slGetFeatureFunction = nullptr;
        PFn_slDLSSSetOptions      g_slDLSSSetOptions = nullptr;

        // 0.5.0: DLSS Ray Reconstruction. Loaded next to DLSS-SR at slInit; while the game's DLSS options, our guide images
        // and the RR options are all in place, the game's DLSS-SR evaluate is run as DLSS-RR instead (fallback: SR).
        using PFn_slDLSSDSetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSDOptions&);
        using PFn_slIsFeatureLoaded = sl::Result (*)(sl::Feature, bool&);
        bool                      g_rrWanted = false;
        std::atomic<bool>         g_rrPaused{ false };  // 0.5.13: Ctrl+Numpad 0 switches RR off live (plain DLSS-SR)
        PFn_slDLSSDSetOptions     g_rrSetOptions = nullptr;
        PFn_slIsFeatureLoaded     g_slIsFeatureLoaded = nullptr;
        std::mutex                g_rrLock;
        sl::DLSSOptions           g_lastDlss{};
        bool                      g_haveDlss = false;
        bool                      g_rrOptionsOk = false;
        bool                      g_rrTagged = false;
        bool                      g_rrLoaded = false;
        bool                      g_rrLookedUp = false;
        bool                      g_rrFeatureOn = false;  // 0.5.25: RR plugin loaded (after the main menu)
        // 0.5.4: when the game last ran DLSS (ms tick). The RR guide pass only runs while DLSS is actually in use.
        std::atomic<std::uint64_t> g_lastDlssEval{ 0 };

        // 0.5.4: support-check hooks, log only. 0.5.3 loaded Streamline 2.14.16 fine but the game never used DLSS:
        // these show which check it failed.
        using PFn_slIsFeatureSupported = sl::Result (*)(sl::Feature, const sl::AdapterInfo&);
        using PFn_slSetFeatureLoaded = sl::Result (*)(sl::Feature, bool);
        using PFn_slGetFeatureRequirements = sl::Result (*)(sl::Feature, sl::FeatureRequirements&);
        using PFn_slGetFeatureVersion = sl::Result (*)(sl::Feature, sl::FeatureVersion&);
        PFn_slIsFeatureSupported      g_slIsFeatureSupported = nullptr;
        PFn_slIsFeatureLoaded         g_slIsFeatureLoadedImp = nullptr;
        PFn_slSetFeatureLoaded        g_slSetFeatureLoaded = nullptr;
        PFn_slGetFeatureRequirements  g_slGetFeatureRequirements = nullptr;
        PFn_slGetFeatureVersion       g_slGetFeatureVersion = nullptr;
        std::atomic<int>              g_checkLogs{ 0 };
        bool CheckLog() { return g_checkLogs.fetch_add(1) < 200; }
        std::atomic<int>          g_rrState{ -1 };   // last logged: 0 = SR, 1 = RR
        sl::Resource              g_rrRes[4]{};      // kept alive for Streamline across the slSetTag call
        sl::Resource              g_cbtRes{};        // 0.5.23: colour before transparency (the game's texture)
        Microsoft::WRL::ComPtr<ID3D12Resource> g_cbtKeep;  // 0.5.23: our reference to it until the next tag
        std::vector<sl::Feature>  g_initFeatures;    // the game's slInit features + RR

        std::mutex g_logLock;

        const char* FeatureName(sl::Feature f)
        {
            switch (f) {
            case sl::kFeatureDLSS: return "DLSS-SR";
            case sl::kFeatureNIS: return "NIS";
            case sl::kFeatureReflex: return "Reflex";
            case sl::kFeaturePCL: return "PCL";
            case sl::kFeatureDeepDVC: return "DeepDVC";
            case sl::kFeatureDLSS_G: return "DLSS-G (frame generation)";
            case sl::kFeatureDLSS_RR: return "DLSS-RR (ray reconstruction)";
            case sl::kFeatureDLSS_NR: return "DLSS-NR";
            case sl::kFeatureImGUI: return "ImGui";
            case sl::kFeatureCommon: return "common";
            default: return "?";
            }
        }

        const char* BufferName(sl::BufferType t)
        {
            switch (t) {
            case sl::kBufferTypeDepth: return "Depth";
            case sl::kBufferTypeMotionVectors: return "MotionVectors";
            case sl::kBufferTypeHUDLessColor: return "HUDLessColor";
            case sl::kBufferTypeScalingInputColor: return "ScalingInputColor";
            case sl::kBufferTypeScalingOutputColor: return "ScalingOutputColor";
            case sl::kBufferTypeNormals: return "Normals";
            case sl::kBufferTypeRoughness: return "Roughness";
            case sl::kBufferTypeAlbedo: return "Albedo";
            case sl::kBufferTypeSpecularAlbedo: return "SpecularAlbedo";
            case sl::kBufferTypeSpecularMotionVectors: return "SpecularMotionVectors";
            case sl::kBufferTypeExposure: return "Exposure";
            case sl::kBufferTypeNormalRoughness: return "NormalRoughness";
            case sl::kBufferTypeUIColorAndAlpha: return "UIColorAndAlpha";
            case sl::kBufferTypeSpecularHitDistance: return "SpecularHitDistance";
            case sl::kBufferTypeColorBeforeTransparency: return "ColorBeforeTransparency";
            case sl::kBufferTypeBiasCurrentColorHint: return "BiasCurrentColorHint";
            case sl::kBufferTypeTransparencyHint: return "TransparencyHint";
            default: return "?";
            }
        }

        // Each distinct (viewport, type, resource, extent) is logged once, so the log shows the tag set without flooding.
        std::set<std::tuple<uint32_t, uint32_t, void*, uint32_t, uint32_t>> g_seenTags;
        std::atomic<uint64_t> g_tagCalls{ 0 }, g_evalCalls{ 0 }, g_constCalls{ 0 };

        // 0.5.1: Streamline's own messages, forwarded into our log (warnings, errors and anything about plugins, so the
        // reason a feature doesn't load is visible). The game gives Streamline no log file.
        sl::PFun_LogMessageCallback* g_gameLogCallback = nullptr;
        void SlLogMessage(sl::LogType type, const char* msg)
        {
            if (g_gameLogCallback) g_gameLogCallback(type, msg);
            if (!msg) return;
            std::string m(msg);
            while (!m.empty() && (m.back() == 10 || m.back() == 13)) m.pop_back();  // trailing newline / carriage return
            std::string lower = m;
            for (auto& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            const bool interesting = type != sl::LogType::eInfo || lower.find("plugin") != std::string::npos ||
                                     lower.find("dlss_d") != std::string::npos || lower.find("dlssd") != std::string::npos ||
                                     lower.find("ngx") != std::string::npos || lower.find("feature") != std::string::npos;
            static std::atomic<int> count{ 0 };
            if (interesting && count.fetch_add(1) < 400) {
                REX::INFO("Starfield RT: [SL log {}] {}", type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warn" : "info", m);
            }
        }

        sl::Result H_slInit(const sl::Preferences& pref, uint64_t sdkVersion)
        {
            {
                std::lock_guard lock(g_logLock);
                std::string features;
                for (uint32_t i = 0; pref.featuresToLoad && i < pref.numFeaturesToLoad; ++i) {
                    features += std::format("{}{} ({})", i ? ", " : "", pref.featuresToLoad[i], FeatureName(pref.featuresToLoad[i]));
                }
                REX::INFO("Starfield RT: [SL] slInit: sdk {:#x}, flags {:#x}, engine {}, application id {}, render API {}, {} plugin path(s), features: {}",
                    sdkVersion, static_cast<uint64_t>(pref.flags), static_cast<int>(pref.engine), pref.applicationId,
                    static_cast<int>(pref.renderAPI), pref.numPathsToPlugins, features.empty() ? std::string("none") : features);
            }
            if (g_rrWanted) {
                g_initFeatures.assign(pref.featuresToLoad, pref.featuresToLoad + (pref.featuresToLoad ? pref.numFeaturesToLoad : 0));
                bool has = false;
                for (auto f : g_initFeatures) has |= f == sl::kFeatureDLSS_RR;
                if (!has) g_initFeatures.push_back(sl::kFeatureDLSS_RR);
                sl::Preferences p = pref;
                p.featuresToLoad = g_initFeatures.data();
                p.numFeaturesToLoad = static_cast<uint32_t>(g_initFeatures.size());
                // 0.5.22: Streamline allocates its own GPU resources instead of calling back into the game. With RR on, the
                // game crashed at startup in its UI code (~1 start in 3; 4-5 clean starts with bDLSSRayReconstruction=0).
                // The game's callbacks were written for Streamline 2.2, and the 2.14 RR plugin allocates many internal
                // resources through them.
                // 0.5.31: reverted. Clearing the game's resource callbacks did not stop the main-menu crash, and with
                // Streamline allocating on its own its plugin manager threw at start-up (0.5.29 / 0.5.30: Reflex lookups
                // -> 24 eErrorInitNotCalled, then std::_Throw_Cpp_error inside sl::plugin_manager::PluginManager).
                REX::INFO("Starfield RT: [SL] game resource callbacks: allocate {:p}, release {:p} (kept)",
                    reinterpret_cast<void*>(pref.allocateCallback), reinterpret_cast<void*>(pref.releaseCallback));
                // 0.5.2: no log-level / callback change any more: with the verbose level (0.5.1) Streamline threw an
                // exception while loading its plugins (2.14.1 core with 2.2.0 plugins in this setup) and slInit failed.
                const sl::Result r = g_slInit(p, sdkVersion);
                REX::INFO("Starfield RT: [SL] slInit (+ DLSS-RR) -> {}", static_cast<int>(r));
                // 0.5.25 unloaded RR again right here. 0.5.30: not during Streamline's own start-up any more: the game's
                // Reflex lookups then failed (24) and Streamline's plugin manager threw (crash at start). RR is unloaded
                // on the first slSetConstants instead (the game's Streamline thread, between frames), and loaded again
                // once we are in game.
                return r;
            }
            const sl::Result r = g_slInit(pref, sdkVersion);
            REX::INFO("Starfield RT: [SL] slInit -> {}", static_cast<int>(r));
            return r;
        }

        sl::Result H_slSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmd)
        {
            g_tagCalls.fetch_add(1);
            for (uint32_t i = 0; tags && i < numTags; ++i) {
                const sl::ResourceTag& t = tags[i];
                void* native = t.resource ? t.resource->native : nullptr;
                const auto key = std::make_tuple(static_cast<uint32_t>(viewport), static_cast<uint32_t>(t.type), native, t.extent.width, t.extent.height);
                std::lock_guard lock(g_logLock);
                if (g_seenTags.size() > 40 || !g_seenTags.insert(key).second) continue;  // 0.5.6: 400 -> 40 (depth is a new buffer most frames)
                std::string desc = "null";
                if (native) {
                    const D3D12_RESOURCE_DESC d = static_cast<ID3D12Resource*>(native)->GetDesc();
                    desc = std::format("{}x{} format {} dim {} flags {:#x}", d.Width, d.Height, static_cast<int>(d.Format), static_cast<int>(d.Dimension),
                        static_cast<int>(d.Flags));
                }
                REX::INFO("Starfield RT: [SL] tag viewport {} {} ({}): {:p} {} state {:#x} lifecycle {} extent {},{} {}x{}",
                    static_cast<uint32_t>(viewport), BufferName(t.type), t.type, native, desc, t.resource ? t.resource->state : 0u,
                    static_cast<int>(t.lifecycle), t.extent.left, t.extent.top, t.extent.width, t.extent.height);
            }
            if (g_rrWanted && tags) {
                bool colour = false;
                for (uint32_t i = 0; i < numTags; ++i) colour |= tags[i].type == sl::kBufferTypeScalingInputColor;
                ID3D12Resource* g[4]{};
                std::uint32_t w = 0, h = 0;
                if (colour && GetRrGuides(g, w, h)) {
                    std::lock_guard lock(g_rrLock);
                    std::vector<sl::ResourceTag> all(tags, tags + numTags);
                    constexpr sl::BufferType kTypes[4] = { sl::kBufferTypeAlbedo, sl::kBufferTypeSpecularAlbedo, sl::kBufferTypeNormalRoughness,
                                                          sl::kBufferTypeSpecularHitDistance };
                    constexpr uint32_t kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    sl::Extent ext{ 0, 0, w, h };
                    for (int i = 0; i < 4; ++i) {
                        g_rrRes[i] = sl::Resource(sl::ResourceType::eTex2d, g[i], kRead);
                        all.emplace_back(&g_rrRes[i], kTypes[i], sl::ResourceLifecycle::eValidUntilPresent, &ext);
                    }
                    // 0.5.23: the game's lit image from just before transparencies, when it matches the input colour's
                    // size and format (RR's guide: a different format makes it see transparency everywhere).
                    g_cbtKeep = GetColorBeforeTransparency();
                    bool cbt = false;
                    if (g_cbtKeep) {
                        const D3D12_RESOURCE_DESC cd = g_cbtKeep->GetDesc();
                        for (uint32_t i = 0; i < numTags; ++i) {
                            if (tags[i].type != sl::kBufferTypeScalingInputColor || !tags[i].resource || !tags[i].resource->native) continue;
                            const D3D12_RESOURCE_DESC id = static_cast<ID3D12Resource*>(tags[i].resource->native)->GetDesc();
                            cbt = cd.Width == id.Width && cd.Height == id.Height && cd.Format == id.Format;
                            static bool logged = false;
                            if (!logged) {
                                logged = true;
                                REX::INFO("Starfield RT: [RR] colour before transparency: {} (source {}x{} format {}, input colour {}x{} format {})",
                                    cbt ? "tagged" : "NOT tagged, size or format differ", cd.Width, cd.Height, static_cast<int>(cd.Format),
                                    id.Width, id.Height, static_cast<int>(id.Format));
                            }
                        }
                    }
                    if (cbt) {
                        g_cbtRes = sl::Resource(sl::ResourceType::eTex2d, g_cbtKeep.Get(), kRead);
                        all.emplace_back(&g_cbtRes, sl::kBufferTypeColorBeforeTransparency, sl::ResourceLifecycle::eValidUntilPresent, &ext);
                    }
                    const sl::Result r = g_slSetTag(viewport, all.data(), static_cast<uint32_t>(all.size()), cmd);
                    if (!g_rrTagged) REX::INFO("Starfield RT: [RR] guide images tagged with the game's DLSS inputs ({}x{}) -> {}", w, h, static_cast<int>(r));
                    g_rrTagged = r == sl::Result::eOk;
                    return r;
                }
            }
            return g_slSetTag(viewport, tags, numTags, cmd);
        }

        sl::Result H_slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs,
            sl::CommandBuffer* cmd)
        {
            const uint64_t n = g_evalCalls.fetch_add(1);
            if (n < 6 || n % 3600 == 0) {
                std::string in;
                for (uint32_t i = 0; inputs && i < numInputs; ++i) {
                    const sl::BaseStructure* b = inputs[i];
                    if (!b) continue;
                    in += std::format(" {{{:08x} v{}}}", b->structType.data1, b->structVersion);
                }
                REX::INFO("Starfield RT: [SL] slEvaluateFeature #{} {} ({}), frame {}, {} input(s):{}", n, feature, FeatureName(feature),
                    static_cast<uint32_t>(frame), numInputs, in);
            }
            if (feature == sl::kFeatureDLSS) g_lastDlssEval = GetTickCount64();
            if (feature == sl::kFeatureDLSS && g_rrWanted) {
                bool use = false;
                {
                    std::lock_guard lock(g_rrLock);
                    use = g_rrOptionsOk && g_rrTagged && g_rrLoaded;
                }
                if (g_rrPaused.load()) use = false;
                if (use) {
                    const sl::Result r = g_slEvaluateFeature(sl::kFeatureDLSS_RR, frame, inputs, numInputs, cmd);
                    if (r == sl::Result::eOk) {
                        if (g_rrState.exchange(1) != 1) REX::INFO("Starfield RT: [RR] DLSS Ray Reconstruction is running in place of DLSS Super Resolution");
                        return r;
                    }
                    static std::atomic<int> failed{ 0 };
                    if (failed.fetch_add(1) < 5) REX::WARN("Starfield RT: [RR] DLSS-RR evaluate failed ({}): DLSS-SR this frame", static_cast<int>(r));
                }
                if (g_rrState.exchange(0) != 0) REX::INFO("Starfield RT: [RR] DLSS Super Resolution (RR not ready: options {}, tags {}, loaded {})",
                    g_rrOptionsOk, g_rrTagged, g_rrLoaded);
            }
            return g_slEvaluateFeature(feature, frame, inputs, numInputs, cmd);
        }

        sl::Result H_slSetConstants(const sl::Constants& c, const sl::FrameToken& frame, const sl::ViewportHandle& viewport)
        {
            const uint64_t n = g_constCalls.fetch_add(1);
            if (n < 2 || n % 3600 == 0) {
                REX::INFO("Starfield RT: [SL] slSetConstants #{} viewport {}: jitter ({:.3f}, {:.3f}), mvecScale ({:g}, {:g}), near {:g} far {:g} fov {:g} "
                          "aspect {:g}, depthInverted {}, cameraMotionIncluded {}, mv3D {}, mvDilated {}, mvJittered {}, reset {}, cameraPos ({:.1f}, {:.1f}, {:.1f})",
                    n, static_cast<uint32_t>(viewport), c.jitterOffset.x, c.jitterOffset.y, c.mvecScale.x, c.mvecScale.y, c.cameraNear, c.cameraFar,
                    c.cameraFOV, c.cameraAspectRatio, static_cast<int>(c.depthInverted), static_cast<int>(c.cameraMotionIncluded),
                    static_cast<int>(c.motionVectors3D), static_cast<int>(c.motionVectorsDilated), static_cast<int>(c.motionVectorsJittered),
                    static_cast<int>(c.reset), c.cameraPos.x, c.cameraPos.y, c.cameraPos.z);
            }
            if (g_rrWanted) {
                std::lock_guard lock(g_rrLock);
                // 0.5.25: load RR only once we are in game (our guide images exist). Done here, on the game's own
                // Streamline thread between frames, like the game's own slSetFeatureLoaded calls.
                if (!g_rrFeatureOn) {
                    ID3D12Resource* gd[4]{};
                    std::uint32_t gw = 0, gh = 0;
                    if (!GetRrGuides(gd, gw, gh) || !g_slSetFeatureLoaded) {
                        static bool unloaded = false;  // 0.5.30: first call (main menu): unload RR until we are in game
                        if (!unloaded && g_slSetFeatureLoaded) {
                            unloaded = true;
                            const sl::Result u = g_slSetFeatureLoaded(sl::kFeatureDLSS_RR, false);
                            REX::INFO("Starfield RT: [RR] DLSS-RR unloaded until in game (first slSetConstants) -> {}", static_cast<int>(u));
                        }
                        return g_slSetConstants(c, frame, viewport);
                    }
                    g_rrFeatureOn = true;
                    const sl::Result u = g_slSetFeatureLoaded(sl::kFeatureDLSS_RR, true);
                    REX::INFO("Starfield RT: [RR] DLSS-RR loaded now that we are in game -> {}", static_cast<int>(u));
                    g_rrLookedUp = false;
                }
                if (!g_rrLookedUp && g_slGetFeatureFunction) {
                    g_rrLookedUp = true;
                    void* fn = nullptr;
                    const sl::Result r = g_slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", fn);
                    g_rrSetOptions = r == sl::Result::eOk ? reinterpret_cast<PFn_slDLSSDSetOptions>(fn) : nullptr;
                    if (HMODULE m = GetModuleHandleW(L"sl.interposer.dll")) {
                        g_slIsFeatureLoaded = reinterpret_cast<PFn_slIsFeatureLoaded>(GetProcAddress(m, "slIsFeatureLoaded"));
                    }
                    bool loaded = false;
                    if (g_slIsFeatureLoaded) g_slIsFeatureLoaded(sl::kFeatureDLSS_RR, loaded);
                    g_rrLoaded = loaded && g_rrSetOptions;
                    REX::INFO("Starfield RT: [RR] DLSS-RR plugin {} (slDLSSDSetOptions {}): {}", loaded ? "loaded" : "NOT loaded",
                        static_cast<int>(r), g_rrLoaded ? "ready" : "staying on DLSS-SR (are sl.dlss_d.dll and nvngx_dlssd.dll next to Starfield.exe?)");
                }
                if (g_rrLoaded && g_haveDlss) {
                    sl::DLSSDOptions o{};
                    o.mode = g_lastDlss.mode;
                    o.outputWidth = g_lastDlss.outputWidth;
                    o.outputHeight = g_lastDlss.outputHeight;
                    o.preExposure = g_lastDlss.preExposure;
                    o.exposureScale = g_lastDlss.exposureScale;
                    o.colorBuffersHDR = sl::Boolean::eTrue;
                    o.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
                    // Row vectors (Streamline convention): cameraViewToWorld rows = right, up, forward, position.
                    sl::float3 r = c.cameraRight, u = c.cameraUp, f = c.cameraFwd;
                    if (r.x == 0 && r.y == 0 && r.z == 0) { r = { 1, 0, 0 }; u = { 0, 1, 0 }; f = { 0, 0, 1 }; }
                    o.cameraViewToWorld = sl::float4x4{ { sl::float4(r.x, r.y, r.z, 0), sl::float4(u.x, u.y, u.z, 0), sl::float4(f.x, f.y, f.z, 0),
                                                          sl::float4(c.cameraPos.x, c.cameraPos.y, c.cameraPos.z, 1) } };
                    const float tx = -(c.cameraPos.x * r.x + c.cameraPos.y * r.y + c.cameraPos.z * r.z);
                    const float ty = -(c.cameraPos.x * u.x + c.cameraPos.y * u.y + c.cameraPos.z * u.z);
                    const float tz = -(c.cameraPos.x * f.x + c.cameraPos.y * f.y + c.cameraPos.z * f.z);
                    o.worldToCameraView = sl::float4x4{ { sl::float4(r.x, u.x, f.x, 0), sl::float4(r.y, u.y, f.y, 0), sl::float4(r.z, u.z, f.z, 0),
                                                          sl::float4(tx, ty, tz, 1) } };
                    const sl::Result res = g_rrSetOptions(viewport, o);
                    if (!g_rrOptionsOk || res != sl::Result::eOk) {
                        static int logged = 0;
                        if (logged++ < 4) {
                            REX::INFO("Starfield RT: [RR] slDLSSDSetOptions mode {} output {}x{} -> {} (camera right ({:.2f}, {:.2f}, {:.2f}) fwd ({:.2f}, {:.2f}, {:.2f}))",
                                static_cast<int>(o.mode), o.outputWidth, o.outputHeight, static_cast<int>(res), r.x, r.y, r.z, f.x, f.y, f.z);
                        }
                    }
                    g_rrOptionsOk = res == sl::Result::eOk;
                }
            }
            return g_slSetConstants(c, frame, viewport);
        }

        sl::Result H_slDLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& o)
        {
            static sl::DLSSMode lastMode = static_cast<sl::DLSSMode>(-1);
            static uint32_t lastW = 0, lastH = 0;
            if (o.mode != lastMode || o.outputWidth != lastW || o.outputHeight != lastH) {
                lastMode = o.mode;
                lastW = o.outputWidth;
                lastH = o.outputHeight;
                REX::INFO("Starfield RT: [SL] slDLSSSetOptions viewport {}: mode {}, output {}x{}, preExposure {:g}, exposureScale {:g}, colorBuffersHDR {}, "
                          "autoExposure {}, presets dlaa {} quality {} balanced {} performance {} ultraPerf {}",
                    static_cast<uint32_t>(viewport), static_cast<int>(o.mode), o.outputWidth, o.outputHeight, o.preExposure, o.exposureScale,
                    static_cast<int>(o.colorBuffersHDR), static_cast<int>(o.useAutoExposure), static_cast<int>(o.dlaaPreset),
                    static_cast<int>(o.qualityPreset), static_cast<int>(o.balancedPreset), static_cast<int>(o.performancePreset),
                    static_cast<int>(o.ultraPerformancePreset));
            }
            {
                std::lock_guard lock(g_rrLock);
                g_lastDlss = o;
                g_haveDlss = true;
            }
            return g_slDLSSSetOptions(viewport, o);
        }

        sl::Result H_slGetFeatureFunction(sl::Feature feature, const char* name, void*& fn)
        {
            const sl::Result r = g_slGetFeatureFunction(feature, name, fn);
            REX::INFO("Starfield RT: [SL] slGetFeatureFunction {} ({}) '{}' -> {}", feature, FeatureName(feature), name ? name : "?", static_cast<int>(r));
            if (r == sl::Result::eOk && fn && name && std::strcmp(name, "slDLSSSetOptions") == 0) {
                g_slDLSSSetOptions = reinterpret_cast<PFn_slDLSSSetOptions>(fn);
                fn = reinterpret_cast<void*>(&H_slDLSSSetOptions);
            }
            return r;
        }

        sl::Result H_slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter)
        {
            const sl::Result r = g_slIsFeatureSupported(feature, adapter);
            if (CheckLog()) REX::INFO("Starfield RT: [SL] slIsFeatureSupported {} ({}) -> {}", feature, FeatureName(feature), static_cast<int>(r));
            return r;
        }

        sl::Result H_slIsFeatureLoaded(sl::Feature feature, bool& loaded)
        {
            const sl::Result r = g_slIsFeatureLoadedImp(feature, loaded);
            if (CheckLog()) REX::INFO("Starfield RT: [SL] slIsFeatureLoaded {} ({}) -> {}, loaded {}", feature, FeatureName(feature), static_cast<int>(r), loaded);
            return r;
        }

        sl::Result H_slSetFeatureLoaded(sl::Feature feature, bool loaded)
        {
            const sl::Result r = g_slSetFeatureLoaded(feature, loaded);
            if (CheckLog()) REX::INFO("Starfield RT: [SL] slSetFeatureLoaded {} ({}) {} -> {}", feature, FeatureName(feature), loaded, static_cast<int>(r));
            return r;
        }

        sl::Result H_slGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& req)
        {
            const sl::Result r = g_slGetFeatureRequirements(feature, req);
            if (CheckLog()) REX::INFO("Starfield RT: [SL] slGetFeatureRequirements {} ({}) -> {}, flags {:#x}, viewports {}, required tags {}", feature,
                FeatureName(feature), static_cast<int>(r), static_cast<uint32_t>(req.flags), req.maxNumViewports, req.numRequiredTags);
            return r;
        }

        sl::Result H_slGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& v)
        {
            const sl::Result r = g_slGetFeatureVersion(feature, v);
            if (CheckLog()) REX::INFO("Starfield RT: [SL] slGetFeatureVersion {} ({}) -> {}, SL {}.{}.{}, NGX {}.{}.{}", feature, FeatureName(feature),
                static_cast<int>(r), v.versionSL.major, v.versionSL.minor, v.versionSL.build, v.versionNGX.major, v.versionNGX.minor, v.versionNGX.build);
            return r;
        }

        // Every function Starfield.exe imports from `dll`, for the log.
        std::string ImportNames(const char* dll)
        {
            std::string out;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) return out;
            for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
                if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                for (; names->u1.AddressOfData; ++names) {
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                    out += ' ';
                    out += reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name);
                }
            }
            return out;
        }

        // Patches the import address table entry of `func` from `dll` in the main executable. Returns the original.
        void* PatchImport(const char* dll, const char* func, void* hook)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) return nullptr;
            for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
                if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++addrs) {
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                    const auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
                    void* original = reinterpret_cast<void*>(addrs->u1.Function);
                    DWORD old = 0;
                    if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
                    addrs->u1.Function = reinterpret_cast<ULONGLONG>(hook);
                    VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
                    return original;
                }
            }
            return nullptr;
        }
    }

    void InstallStreamlineLogging(bool rayReconstruction)
    {
        static bool done = false;
        if (done) return;
        done = true;
        g_rrWanted = rayReconstruction;
        constexpr const char* kDll = "sl.interposer.dll";
        g_slInit = reinterpret_cast<PFn_slInit>(PatchImport(kDll, "slInit", reinterpret_cast<void*>(&H_slInit)));
        g_slSetTag = reinterpret_cast<PFn_slSetTag>(PatchImport(kDll, "slSetTag", reinterpret_cast<void*>(&H_slSetTag)));
        g_slEvaluateFeature = reinterpret_cast<PFn_slEvaluateFeature>(PatchImport(kDll, "slEvaluateFeature", reinterpret_cast<void*>(&H_slEvaluateFeature)));
        g_slSetConstants = reinterpret_cast<PFn_slSetConstants>(PatchImport(kDll, "slSetConstants", reinterpret_cast<void*>(&H_slSetConstants)));
        g_slGetFeatureFunction = reinterpret_cast<PFn_slGetFeatureFunction>(
            PatchImport(kDll, "slGetFeatureFunction", reinterpret_cast<void*>(&H_slGetFeatureFunction)));
        g_slIsFeatureSupported = reinterpret_cast<PFn_slIsFeatureSupported>(
            PatchImport(kDll, "slIsFeatureSupported", reinterpret_cast<void*>(&H_slIsFeatureSupported)));
        g_slIsFeatureLoadedImp = reinterpret_cast<PFn_slIsFeatureLoaded>(PatchImport(kDll, "slIsFeatureLoaded", reinterpret_cast<void*>(&H_slIsFeatureLoaded)));
        g_slSetFeatureLoaded = reinterpret_cast<PFn_slSetFeatureLoaded>(PatchImport(kDll, "slSetFeatureLoaded", reinterpret_cast<void*>(&H_slSetFeatureLoaded)));
        g_slGetFeatureRequirements = reinterpret_cast<PFn_slGetFeatureRequirements>(
            PatchImport(kDll, "slGetFeatureRequirements", reinterpret_cast<void*>(&H_slGetFeatureRequirements)));
        g_slGetFeatureVersion = reinterpret_cast<PFn_slGetFeatureVersion>(
            PatchImport(kDll, "slGetFeatureVersion", reinterpret_cast<void*>(&H_slGetFeatureVersion)));
        REX::INFO("Starfield RT: [SL] Starfield imports from {}:{}", kDll, ImportNames(kDll));
        REX::INFO("Starfield RT: [SL] Streamline hooks (Ray Reconstruction {}): slInit {}, slSetTag {}, slEvaluateFeature {}, slSetConstants {}, slGetFeatureFunction {}",
            g_rrWanted ? "wanted" : "off", g_slInit != nullptr, g_slSetTag != nullptr, g_slEvaluateFeature != nullptr, g_slSetConstants != nullptr, g_slGetFeatureFunction != nullptr);
    }

    bool RayReconstructionWanted() { return g_rrWanted; }

    bool ToggleRrPaused() { return !g_rrPaused.exchange(!g_rrPaused.load()); }
    void SetRrPaused(bool paused) { g_rrPaused = paused; }

    bool RayReconstructionRunning() { return g_rrWanted && g_rrState.load() == 1 && DlssRunning(); }

    bool DlssRunning()
    {
        const std::uint64_t last = g_lastDlssEval.load();
        return last && GetTickCount64() - last < 2000;
    }
}
