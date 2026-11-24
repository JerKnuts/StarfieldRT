// Starfield RT - Phase 0.
// 0.1.1: found that passes record straight into D3D12 command lists (through Streamline).
// 0.1.3: found that ShadowMaskRenderPass is a 64x64 depth-only caster mask, not the sun shadow mask.
// 0.1.4: pass survey. Hooks Execute (vtable slot 7) of several candidate passes and logs what each
//        one records: draws/dispatches, bound targets, and every resource barrier with the resource's
//        size/format/states. Goal: find the screen-space buffers where sun shadowing and lighting land.
// 0.1.5: optional PIX GPU capturer loading ([Debug] bLoadPixGpuCapturer) so PIX can attach.
// 0.1.6: points PIX at the game window (SetGlobalTargetWindow) and adds an in-game hotkey
//        (default F10) that takes a programmatic one-frame GPU capture to Documents\StarfieldRT Captures.
// 0.1.8: first write into the lighting: a checker pattern dispatched into the contact shadow mask
//        right after TiledDeferredContactShadowsBlurY ([Debug] iMaskTest).
// Read-only: nothing is written into the frame.

#include <SFSE/SFSE.h>
#include <RE/C/CreationRenderer.h>
#include <RE/I/INIPrefSettingCollection.h>
#include <RE/I/INISettingCollection.h>
#include <RE/T/TESGlobal.h>
#include <RE/B/BSTEvent.h>        // 0.3.1: on-screen hotkey messages
#include <RE/B/BSFixedString.h>
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "generated/MaskTestPatternCS.h"  // compiled from shaders/MaskTestPattern.hlsl with dxc
#include "DeviceHooks.h"
#include "Blas.h"
#include "RtShadows.h"
#include "Reflections.h"
#include "GpuTimer.h"
#include "StreamlineHooks.h"  // 0.4.11

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;
    constexpr REL::Version kSupportedRuntime{ 1, 16, 244, 0 };

    // ---------------------------------------------------------------- pass table (1.16.244 RVAs)

    struct PassHook
    {
        const char*       name;
        std::uintptr_t    vtableRva;   // render pass vtable
        std::uintptr_t    executeRva;  // expected stock pointer in slot 7
        void*             original = nullptr;
        std::atomic<int>  logged{ 0 };
    };

    constexpr std::size_t kExecuteSlot = 7;
    constexpr int         kLogsPerPass = 3;
    constexpr std::size_t kMaxBarriersLogged = 12;

    PassHook g_passes[] = {
        { "ShadowMask", 0x4EE48F0, 0x2B911D0 },
        { "ScreenSpaceShadows", 0x4DB4EA0, 0x29AC5F0 },
        { "TiledDeferredContactShadows", 0x4DB4EE8, 0x29ACA60 },
        { "TiledDeferredContactShadowsBlurX", 0x4DB4F30, 0x29AD0E0 },
        { "TiledDeferredContactShadowsBlurY", 0x4DB4F78, 0x29AD5B0 },
        { "TiledDeferredLightingProcessLights", 0x4EE2CA8, 0x2B6B0C0 },
        { "Lighting", 0x4EE1168, 0x2A53BB0 },
        { "SkyLighting", 0x4EE2890, 0x2B668E0 },
        { "SAO", 0x4DB5F18, 0x2A53BB0 },
        { "DeferredGBuffer", 0x4EDF578, 0x2B32020 },
        { "DeferredComposite", 0x4EDF480, 0x2B31710 },
        { "SunDisk", 0x4EDD650, 0x2B07940 },
        { "ThinDeferredLighting", 0x4EDE1C0, 0x2B1ED00 },
        { "TiledBinning_idTech7SampleIndirectLighting", 0x4ED5E10, 0x2A73C80 },
        { "GatherGeometryInstances", 0x4DBAA38, 0x2A238B0 },
        { "GatherTransforms", 0x4ED63A8, 0x2A784B0 },
        // 0.2.5: shadow-map passes (vtables from RTTI), surveyed to find the one that draws the sun cascades.
        { "ShadowMapInstance", 0x4EE1FA8, 0x2B58E50 },
        { "GroupedDynamicShadowMap", 0x4EE2090, 0x2B63240 },
        { "ClearShadowMaps", 0x4ED7800, 0x2B5E340 },
        { "ClearShadowMapGroup", 0x4ED77B8, 0x2B5E990 },
        { "TerrainShadows", 0x4DB2E10, 0x2996780 },
        { "SetupDynamicShadow", 0x4EE2168, 0x2B5CE60 },
        // 0.2.22: R1 of RT reflections. GatherIndirect draws Indirect#0 (indirect diffuse + probe specular).
        { "GatherIndirect", 0x4DB3C48, 0x299F180 },
        // 0.2.38: G1 glass survey. Transparent and forward passes (vtables from CommonLibSF IDs_VTABLE.h + the 1.16.244
        // address library, Execute read from the exe; tools/pass_vtables.py). Glass draws LayeredEffect with
        // moment-based OIT: moments (half res), transparency (RGBA16), then CompositeMomentBasedOIT into the HDR target.
        { "Forward", 0x4ED9CE8, 0x2ABEEA0 },
        { "ForwardBlendedMainFullRes", 0x4ED9D30, 0x2ABF750 },
        { "ForwardBlendedMainHalfRes", 0x4ED9AA8, 0x2AC0C30 },
        { "ForwardBlendedFullResAlways", 0x4ED9A60, 0x2AC01C0 },
        { "UnsortedForwardBlendedPreMBOIT", 0x4ED9B80, 0x2ABF750 },
        { "UnsortedForwardBlendedPostMBOIT", 0x4ED9BC8, 0x2ABF750 },
        { "UnsortedForwardBlendedPreClouds", 0x4ED9B38, 0x2AC2210 },
        { "MomentBasedOITRenderMoments", 0x4ED9C10, 0x2AC2BF0 },
        { "MomentBasedOITRenderTransparency", 0x4ED9C58, 0x2AC3660 },
        { "MomentBasedOITRenderAdditiveTransparency", 0x4ED9D78, 0x2AC4170 },
        { "CompositeMomentBasedOIT", 0x4DB9288, 0x2A02720 },
        { "DeferredGBufferTranslucent", 0x4EDF530, 0x2AC5D80 },
        { "RefractionMask", 0x4ED9F10, 0x2AC5FA0 },
        { "ForwardBlendedCorrection", 0x4ED9AF0, 0x2AC16A0 },
        { "ForwardBlendedCopyToDest", 0x4ED9CA0, 0x2B7AAE0 },
        { "ForwardBlendedAlphaOutput", 0x4ED9E20, 0x2AC5350 },
    };
    constexpr std::size_t kFirstGlassPass = 23;  // index of "Forward" above (0.2.38)
    constexpr std::size_t kShadowCandidateEnd = 22;   // one past SetupDynamicShadow (F3 skip-test range)
    constexpr std::size_t kFirstShadowCandidate = 16;  // index of ShadowMapInstance above
    constexpr std::size_t kSunShadowMapPass = 16;      // 0.2.6: ShadowMapInstance draws the sun cascades (4096x4096 DSV)
    constexpr std::size_t kPassCount = std::size(g_passes);

    // ---------------------------------------------------------------- settings / PIX

    int g_sunToggleKey = VK_NUMPAD3;  // [RT] iSunToggleKey: live on/off for RT sun shadows (0 = disabled; 0.4.1: was F11)
    int g_reflBrighterKey = VK_ADD;    // [Debug] iReflectionBrighterKey: 0.2.28 live reflection brightness up (0.4.1: Numpad +)
    int g_reflDarkerKey = VK_SUBTRACT; // [Debug] iReflectionDarkerKey: down (0.4.1: Numpad -)
    int g_reflectDebugKey = VK_NUMPAD0;  // [Debug] iReflectionDebugKey: 0.2.25 R2 debug views (0 = disabled; 0.4.1: was F2)
    int g_probeKey = VK_DECIMAL;  // [Debug] iProbeKey: log what rays through the screen centre hit (0 = disabled; 0.4.1: was F8)
    int g_alphaToggleKey = VK_MULTIPLY; // [RT] iAlphaToggleKey: live on/off for alpha-tested RT shadows (0 = disabled; 0.4.1: was F7)
    std::atomic<bool> g_hideGlassProbe{ true };  // [RT] bHideGameGlassReflections: 0.2.39 (G2), toggled live with iGlassReflectionKey
    int g_glassReflKey = VK_NUMPAD8;             // [RT] iGlassReflectionKey (0.2.41: Numpad 8, was Home)
    std::atomic<bool> g_sharpGlass{ true };      // [RT] bSharpGlass: 0.2.42, toggled live with iSharpGlassKey
    int g_sharpGlassKey = VK_NUMPAD9;            // [RT] iSharpGlassKey
    std::atomic<bool> g_skipThickGlassLayer{ true };  // [RT] bHideThickGlassLayer: 0.2.48, toggled with iThickGlassLayerKey
    int g_thickGlassKey = VK_NUMPAD6;                 // [RT] iThickGlassLayerKey
    int g_glassSeparateKey = VK_NUMPAD7;
    bool g_onScreenMessages = true;                   // [RT] bOnScreenMessages: 0.3.1 hotkeys show what they switched on the HUD
    int g_rtLampKey = VK_NUMPAD2;                     // [RT] iRTLampKey: 0.4.0 RT lamps on / off (game lamps)
    int g_giKey = VK_NUMPAD1;                         // [RT] iGIKey: 0.3.0 RT GI on -> debug -> off
    int g_localLightKey = VK_NUMPAD5;                 // [RT] iLocalLightKey: 0.2.58 game lights in reflections on/off
    int g_thinGlassKey = VK_NUMPAD4;                  // [RT] iThinGlassKey: 0.2.51 thin-glass RT reflections on/off         // [RT] iGlassSeparateKey: 0.2.44 glass on/off for shadow and reflection rays
    int g_softToggleKey = VK_DIVIDE;  // [RT] iSoftToggleKey: live on/off for soft RT shadows (0 = disabled; 0.4.1: was F6)
    int g_litTestKey = 0;         // [Debug] iLitTestKey: 0.2.4 lit test (RT forced to lit, game sun shadows off); 0.4.1: no default key
    int g_skipCycleKey = 0;       // [Debug] iSkipPassKey: 0.2.5 cycles which shadow-map pass has its draws dropped; 0.4.1: no default key
    std::atomic<int> g_skipPass{ -1 };  // g_passes index whose draws are dropped while RT is on (-1 = none)
    thread_local bool t_suppressDraws = false;
    int   g_replaceGameSunShadows = 2;     // [RT] bReplaceGameSunShadows: 0 keep the game's sun cascades; 2 drop the cascade
                                           // pass's draws (default again since 0.2.21); 3 re-clear the cascade maps after their
                                           // pass (0.2.10). 1 (patch SceneShadowData, 0.2.11-0.2.20) is gone and acts as 2.
    float g_gameSunShadowDistanceRT = 0.1f; // [RT] fGameSunShadowDistanceWithRT: fMaxShadowDistance while RT is on
    int g_maskTestMode = 0;   // [Debug] iMaskTest: 0 off, 1 checker cells forced to shadow, 2 forced to lit
    int g_maskTestCell = 64;  // [Debug] iMaskTestCell: checker cell size in pixels
    thread_local std::uint32_t t_dispatchCount = 0;  // engine dispatches recorded on this thread

    std::filesystem::path PluginDirectory()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PluginDirectory), &self);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    // Loads PIX's GPU capturer into the game before the D3D12 device exists, so PIX can
    // attach to a game launched normally through MO2/SFSE ("Attach" -> GPU capture).
    // Picks the newest installed PIX version under %ProgramFiles%\Microsoft PIX.
    void LoadPixGpuCapturer()
    {
        if (GetModuleHandleW(L"WinPixGpuCapturer.dll")) {
            REX::INFO("Starfield RT: PIX GPU capturer already loaded");
            return;
        }
        wchar_t programFiles[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH)) {
            REX::WARN("Starfield RT: PIX capturer: %ProgramFiles% not set");
            return;
        }
        const std::filesystem::path pixRoot = std::filesystem::path(programFiles) / L"Microsoft PIX";
        std::error_code ec;
        std::filesystem::path newest;
        for (const auto& entry : std::filesystem::directory_iterator(pixRoot, ec)) {
            if (entry.is_directory() && std::filesystem::exists(entry.path() / L"WinPixGpuCapturer.dll") &&
                (newest.empty() || entry.path().filename().wstring() > newest.filename().wstring())) {
                newest = entry.path();
            }
        }
        if (newest.empty()) {
            REX::WARN("Starfield RT: PIX capturer requested but no PIX install found under {}", pixRoot.string());
            return;
        }
        const auto dll = newest / L"WinPixGpuCapturer.dll";
        if (LoadLibraryW(dll.c_str())) {
            REX::INFO("Starfield RT: loaded PIX GPU capturer from {} - attach PIX to Starfield.exe to capture", dll.string());
        } else {
            REX::WARN("Starfield RT: failed to load {} ({})", dll.string(), GetLastError());
        }
    }

    // ---- PIX programmatic capture (exports of WinPixGpuCapturer.dll, PIX 2610.28) ----
    // BeginProgrammaticGpuCapture takes a PIXCaptureParameters union whose GPU member is
    // { PCWSTR FileName; }. EndProgrammaticGpuCapture takes no arguments.
    using PixBeginFn = HRESULT(WINAPI*)(const void* captureParameters);
    using PixEndFn = HRESULT(WINAPI*)();
    using PixTargetWindowFn = HRESULT(WINAPI*)(HWND);

    PixBeginFn       g_pixBegin = nullptr;
    PixEndFn         g_pixEnd = nullptr;
    int              g_pixCaptureKey = 0;  // 0.4.1: no default key (was F10)
    constexpr int    kCaptureFrames = 2;  // DeferredComposite boundaries to span after Begin

    struct alignas(8) PixCaptureParameters
    {
        const wchar_t* fileName = nullptr;
        std::byte      reserved[120]{};  // rest of the union (timing-capture members), zeroed
    };

    void ResolvePixExports()
    {
        if (HMODULE pix = GetModuleHandleW(L"WinPixGpuCapturer.dll")) {
            g_pixBegin = reinterpret_cast<PixBeginFn>(GetProcAddress(pix, "BeginProgrammaticGpuCapture"));
            g_pixEnd = reinterpret_cast<PixEndFn>(GetProcAddress(pix, "EndProgrammaticGpuCapture"));
            REX::INFO("Starfield RT: PIX programmatic capture {} (press key {:#x} in-game)",
                g_pixBegin && g_pixEnd ? "available" : "unavailable", g_pixCaptureKey);
        }
    }

    // The game's main window: the largest visible top-level window owned by this process.
    HWND FindGameWindow()
    {
        struct Search { DWORD pid; HWND best; LONG bestArea; } s{ GetCurrentProcessId(), nullptr, 0 };
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != s.pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
                return TRUE;
            }
            RECT r{};
            GetClientRect(hwnd, &r);
            const LONG area = (r.right - r.left) * (r.bottom - r.top);
            if (area > s.bestArea) {
                s.best = hwnd;
                s.bestArea = area;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&s));
        return s.best;
    }

    void SetPixTargetWindow()
    {
        HMODULE pix = GetModuleHandleW(L"WinPixGpuCapturer.dll");
        if (!pix) {
            return;
        }
        auto setTarget = reinterpret_cast<PixTargetWindowFn>(GetProcAddress(pix, "SetGlobalTargetWindow"));
        HWND hwnd = FindGameWindow();
        if (setTarget && hwnd) {
            // GetClassNameW reads window data without sending a message. (0.1.6 used GetWindowTextW,
            // which sends WM_GETTEXT to the main thread and deadlocked against the render thread.)
            wchar_t cls[128]{};
            GetClassNameW(hwnd, cls, 128);
            const HRESULT hr = setTarget(hwnd);
            REX::INFO("Starfield RT: PIX target window {:p} class '{}' -> hr {:#x}", static_cast<void*>(hwnd),
                std::filesystem::path(cls).string(), static_cast<unsigned>(hr));
        } else {
            REX::WARN("Starfield RT: could not set PIX target window (export {}, window {})", setTarget != nullptr,
                hwnd != nullptr);
        }
    }

    std::wstring NewCapturePath()
    {
        wchar_t profile[MAX_PATH]{};
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        const auto dir = std::filesystem::path(profile) / L"Documents" / L"StarfieldRT Captures";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        SYSTEMTIME t{};
        GetLocalTime(&t);
        wchar_t name[64]{};
        swprintf_s(name, L"Starfield_%04u-%02u-%02u_%02u-%02u-%02u.wpix", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return (dir / name).wstring();
    }

    // Called once per frame from the DeferredComposite hook.
    void PixFrameTick()
    {
        static bool         keyWasDown = false;
        static int          framesLeft = -1;  // -1 idle, >0 capturing
        static std::wstring path;
        static PixCaptureParameters params;
        if (!g_pixBegin || !g_pixEnd) {
            return;
        }
        if (framesLeft > 0 && --framesLeft == 0) {
            const HRESULT hr = g_pixEnd();
            REX::INFO("Starfield RT: PIX capture ended (hr {:#x}) -> {}", static_cast<unsigned>(hr),
                std::filesystem::path(path).string());
            framesLeft = -1;
        }
        const bool down = (GetAsyncKeyState(g_pixCaptureKey) & 0x8000) != 0;
        if (down && !keyWasDown && framesLeft < 0) {
            path = NewCapturePath();
            params = {};
            params.fileName = path.c_str();
            const HRESULT hr = g_pixBegin(&params);
            REX::INFO("Starfield RT: PIX capture begin (hr {:#x}) -> {}", static_cast<unsigned>(hr),
                std::filesystem::path(path).string());
            framesLeft = SUCCEEDED(hr) ? kCaptureFrames : -1;
        }
        keyWasDown = down;
    }

    // ---------------------------------------------------------------- quality levels (0.2.34; live since 0.2.36)

    // The ini's individual settings are the base; a quality level (0 off .. 4 ultra) overrides parts of it.
    // -1 = custom: the base unchanged. Applied at startup from the ini and again whenever Gameplay Options change.
    rt::BlasSettings        g_baseBlas;
    rt::SunShadowSettings   g_baseSun;
    rt::ReflectionSettings  g_baseRefl;
    int                     g_iniReflQuality = 3, g_iniShadowQuality = 3;
    int                     g_appliedRefl = -2, g_appliedShadow = -2;

    // 0.5.28: baseline = every effect off and the game's own look everywhere: the game's reflections, GI, lamps and sun
    // shadows, no thin-glass pass, no glass tweaks, Ray Reconstruction paused (plain DLSS), reflection gloss / strength 1.
    // [RT] bStartAllOff applies it at startup and after every quality change (Gameplay Options re-applies all modes);
    // Ctrl+Numpad . applies it live. Each effect's own key then turns it back on, one at a time.
    std::atomic<bool> g_baselineOff{ false };

    void ApplyBaselineOff(const char* why)
    {
        rt::SetReflectionModeRaw(0);
        rt::SetGiModeRaw(0);
        rt::SetDirectLightsRaw(0);
        rt::SetSunShadowsLive(0);
        rt::SetThinGlassMode(0);
        // 0.5.35: gloss / strength keep their ini values (the tester's tuned 0.65 / 0.5); the baseline no longer resets them.
        rt::SetRrPaused(true);
        g_hideGlassProbe = false;
        g_sharpGlass = false;
        g_skipThickGlassLayer = false;
        REX::INFO("Starfield RT: baseline ({}): every effect off (reflections, GI, RT lamps, RT sun shadows, thin glass, glass tweaks), "
                  "Ray Reconstruction paused", why);
    }

    void ApplyQuality(int reflQuality, int shadowQuality, const char* source)
    {
        rt::BlasSettings blas = g_baseBlas;
        rt::SunShadowSettings sun = g_baseSun;
        rt::ReflectionSettings refl = g_baseRefl;
        if (reflQuality >= 0) {
            // 0.5.11: high 48 within 30 -> 64 within 40, ultra 64 within 40 -> 128 within 60 (NPCs popped into RT up close).
            constexpr float kSkinRadius[5] = { 20.0f, 10.0f, 20.0f, 40.0f, 60.0f };
            constexpr int   kSkinSlots[5] = { 32, 16, 32, 64, 128 };
            constexpr int   kScale[5] = { 2, 2, 2, 1, 1 };
            constexpr float kLength[5] = { 1000.0f, 1000.0f, 2000.0f, 3000.0f, 6000.0f };
            blas.skinRadius = kSkinRadius[reflQuality];
            blas.skinSlots = kSkinSlots[reflQuality];
            refl.startOn = reflQuality > 0;
            refl.traceScale = kScale[reflQuality];
            refl.maxDistance = kLength[reflQuality];
            refl.hitShadows = reflQuality >= 2;
            // 0.4.8: GI and RT lamps follow the reflection level too. Ultra = everything at full quality.
            constexpr int kGiScale[5] = { 2, 2, 2, 2, 1 };
            constexpr int kGiRays[5] = { 1, 1, 1, 1, 2 };
            constexpr int kLampShadows[5] = { 2, 2, 4, 4, 8 };
            refl.giTraceScale = kGiScale[reflQuality];
            refl.giRays = kGiRays[reflQuality];
            refl.localShadowRays = kLampShadows[reflQuality];
        }
        if (shadowQuality >= 0) {  // High = the 0.2.x defaults
            constexpr int kRays[5] = { 0, 1, 2, 4, 8 };
            constexpr int kScale[5] = { 2, 2, 2, 2, 1 };
            constexpr int kLength[5] = { 0, 1000, 1500, 2000, 3000 };
            if (shadowQuality == 0) {
                sun.mode = 0;
            } else {
                if (sun.mode == 0) sun.mode = 2;
                sun.rays = kRays[shadowQuality];
                sun.softScale = kScale[shadowQuality];
                sun.maxDistance = static_cast<float>(kLength[shadowQuality]);
            }
        }
        if (g_appliedRefl != -2) {
            refl.probeAmbient = rt::AdjustReflectionBrightness(1.0f);  // keep PageUp/PageDown changes
            refl.glossRoughScale = rt::AdjustReflectionGloss(0.0f);     // 0.5.24: and the live gloss / strength
            refl.glossStrength = rt::AdjustReflectionStrength(0.0f);
        }
        rt::SetBlasSettings(blas);
        rt::SetSunShadowSettings(sun);
        rt::SetReflectionSettings(refl);
        g_appliedRefl = reflQuality;
        g_appliedShadow = shadowQuality;
        static constexpr const char* kLevel[6] = { "custom", "off", "low", "medium", "high", "ultra" };
        REX::INFO("Starfield RT: quality ({}): reflections {} (on {}, 1 ray per {}x{}, length {}, hit shadows {}, characters {} within {}), "
                  "GI 1 cell per {}x{} with {} ray(s), lamp shadows {}, shadows {} (mode {}, {} rays, length {})", source,
            kLevel[reflQuality + 1], refl.startOn, refl.traceScale, refl.traceScale, refl.maxDistance, refl.hitShadows, blas.skinSlots,
            blas.skinRadius, refl.giTraceScale, refl.giTraceScale, refl.giRays, refl.localShadowRays, kLevel[shadowQuality + 1], sun.mode,
            sun.rays, sun.maxDistance);
        if (g_baselineOff.load()) ApplyBaselineOff(source);  // 0.5.28
    }

    // 0.2.36: Gameplay Options (StarfieldRT.esm). Its quest script copies the two options into these globals on every
    // change and save load; the render thread reads them every 30 frames. An ini level of -1 (custom) ignores the menu.
    // 0.5.20: the two option globals are looked up on the game's main thread only (SFSE data-loaded message, then SFSE
    // tasks). Before, a render thread called TESForm::LookupByEditorID every 120 frames until it found them: it walked
    // the game's form table while the main thread was still filling it at startup and at the end of every load. Each load
    // crash of 2026-11-21 came within ~2 s of that lookup (the "Gameplay Options found" line).
    std::atomic<RE::TESGlobal*> g_optRefl{ nullptr }, g_optShadow{ nullptr };
    std::atomic<bool>           g_optLookupQueued{ false };

    void LookupGameOptionsMainThread()
    {
        auto* refl = RE::TESForm::LookupByEditorID<RE::TESGlobal>("SFRT_ReflectionQuality");
        auto* shadow = RE::TESForm::LookupByEditorID<RE::TESGlobal>("SFRT_ShadowQuality");
        if (refl && shadow) {
            g_optRefl = refl;
            g_optShadow = shadow;
            REX::INFO("Starfield RT: Gameplay Options found (StarfieldRT.esm, main thread): reflections {}, shadows {}", refl->GetValue(),
                shadow->GetValue());
        }
        g_optLookupQueued = false;
    }

    void PollGameOptions(std::uint64_t frame)
    {
        static bool reported = false, badValue = false;
        if (frame % 30 != 0) return;
        RE::TESGlobal* refl = g_optRefl.load();
        RE::TESGlobal* shadow = g_optShadow.load();
        if (!refl || !shadow) {
            // Not found at data load: ask the main thread to look again now and then (never from this thread).
            if (frame % 600 == 0 && !g_optLookupQueued.exchange(true)) {
                if (const auto* tasks = SFSE::GetTaskInterface()) {
                    tasks->AddTask([] { LookupGameOptionsMainThread(); });
                } else {
                    g_optLookupQueued = false;
                }
            }
            if (!reported && frame >= 3600) {
                reported = true;
                REX::INFO("Starfield RT: StarfieldRT.esm not active, quality comes from the ini only");
            }
            return;
        }
        auto level = [](RE::TESGlobal* g, int ini, int current) {
            if (ini < 0) return ini;  // custom: the ini decides
            const float v = g->GetValue();
            if (!(v >= -0.01f && v <= 4.01f) || std::abs(v - std::round(v)) > 0.01f) {
                if (!badValue) {
                    badValue = true;
                    REX::WARN("Starfield RT: Gameplay Options global holds {}, not a level; ignoring the menu", v);
                }
                return current;
            }
            return static_cast<int>(std::lround(v));
        };
        const int r = level(refl, g_iniReflQuality, g_appliedRefl);
        const int s = level(shadow, g_iniShadowQuality, g_appliedShadow);
        if (r != g_appliedRefl || s != g_appliedShadow) ApplyQuality(r, s, "Gameplay Options");
    }

    void LoadSettingsAndTools()
    {
        const auto ini = (PluginDirectory() / L"StarfieldRT.ini").wstring();
        const bool pix = GetPrivateProfileIntW(L"Debug", L"bLoadPixGpuCapturer", 0, ini.c_str()) != 0;
        g_pixCaptureKey = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iPixCaptureKey", 0, ini.c_str()));
        g_maskTestMode = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iMaskTest", 0, ini.c_str()));
        g_maskTestCell = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iMaskTestCell", 64, ini.c_str()));
        REX::INFO("Starfield RT: settings bLoadPixGpuCapturer={} iPixCaptureKey={:#x} iMaskTest={} iMaskTestCell={}",
            pix, g_pixCaptureKey, g_maskTestMode, g_maskTestCell);
        if (pix) {
            LoadPixGpuCapturer();
            ResolvePixExports();
        }
        rt::BlasSettings blas;
        blas.enabled = GetPrivateProfileIntW(L"RT", L"bEnableBLAS", 1, ini.c_str()) != 0;
        blas.buildsPerFrame = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iBlasBuildsPerFrame", 128, ini.c_str()));
        blas.budgetMB = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iBlasBudgetMB", 2048, ini.c_str()));
        blas.tlasEnabled = GetPrivateProfileIntW(L"RT", L"bEnableTLAS", 1, ini.c_str()) != 0;
        blas.poolOnly = GetPrivateProfileIntW(L"RT", L"bBlasPoolOnly", 0, ini.c_str()) != 0;  // 0.1.27: default 0
        blas.tlasMaxInstances = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iTlasMaxInstances", 524288, ini.c_str()));
        blas.tlasCullDistance = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iTlasCullDistance", 1500, ini.c_str()));
        blas.scanInterval = (std::max)(1, static_cast<int>(GetPrivateProfileIntW(L"RT", L"iBlasScanInterval", 10, ini.c_str())));  // 0.5.11
        blas.skinRadius = static_cast<float>(GetPrivateProfileIntW(L"RT", L"iSkinnedRadius", 20, ini.c_str()));
        blas.skinSlots = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSkinnedMax", 32, ini.c_str()));
        // 0.2.34: quality levels. -1 = custom (the individual settings below), 0 off, 1 low, 2 medium, 3 high, 4 ultra.
        const int reflQuality = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"RT", L"iReflectionQuality", 3, ini.c_str())), -1, 4);
        const int shadowQuality = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"RT", L"iShadowQuality", 3, ini.c_str())), -1, 4);
        blas.tlasMaxExtent = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iTlasMaxInstanceSize", 1000, ini.c_str()));
        blas.alphaTest = GetPrivateProfileIntW(L"RT", L"bAlphaTest", 1, ini.c_str()) != 0;
        blas.glassSeparate = GetPrivateProfileIntW(L"RT", L"bGlassSeparate", 1, ini.c_str()) != 0;  // 0.2.44
        g_glassSeparateKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iGlassSeparateKey", VK_NUMPAD7, ini.c_str()));
        g_skipThickGlassLayer = GetPrivateProfileIntW(L"RT", L"bHideThickGlassLayer", 1, ini.c_str()) != 0;  // 0.2.48
        g_thickGlassKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iThickGlassLayerKey", VK_NUMPAD6, ini.c_str()));
        g_alphaToggleKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iAlphaToggleKey", VK_MULTIPLY, ini.c_str()));
        g_replaceGameSunShadows = static_cast<int>(GetPrivateProfileIntW(L"RT", L"bReplaceGameSunShadows", 2, ini.c_str()));
        if (g_replaceGameSunShadows == 1) g_replaceGameSunShadows = 2;  // 0.2.21: the SceneShadowData patch is gone
        {
            wchar_t buf[32]{};
            GetPrivateProfileStringW(L"RT", L"fGameSunShadowDistanceWithRT", L"0.1", buf, 32, ini.c_str());
            g_gameSunShadowDistanceRT = (std::max)(0.001f, static_cast<float>(_wtof(buf)));
        }
        g_baseBlas = blas;
        rt::SunShadowSettings sun;
        sun.mode = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSunShadows", 0, ini.c_str()));
        sun.maxDistance = static_cast<float>(GetPrivateProfileIntW(L"RT", L"iSunRayLength", 2000, ini.c_str()));
        sun.biasBase = static_cast<float>(GetPrivateProfileIntW(L"RT", L"iSunBiasCm", 10, ini.c_str())) / 100.0f;
        sun.biasPerUnit = static_cast<float>(GetPrivateProfileIntW(L"RT", L"iSunBiasPerKm", 2, ini.c_str())) / 1000.0f;
        sun.soft = GetPrivateProfileIntW(L"RT", L"bSoftShadows", 1, ini.c_str()) != 0;
        sun.rays = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSunShadowRays", 4, ini.c_str()));
        sun.maxBlurPx = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSoftShadowMaxBlur", 4, ini.c_str()));
        sun.softScale = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSoftShadowScale", 2, ini.c_str()));
        {
            wchar_t buf[32]{};
            GetPrivateProfileStringW(L"RT", L"fSunAngularRadiusDeg", L"0.15", buf, 32, ini.c_str());
            sun.sunRadiusDeg = static_cast<float>(_wtof(buf));
        }
        g_softToggleKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSoftToggleKey", VK_DIVIDE, ini.c_str()));
        g_hideGlassProbe = GetPrivateProfileIntW(L"RT", L"bHideGameGlassReflections", 1, ini.c_str()) != 0;  // 0.2.39 (G2)
        g_glassReflKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iGlassReflectionKey", VK_NUMPAD8, ini.c_str()));
        g_sharpGlass = GetPrivateProfileIntW(L"RT", L"bSharpGlass", 1, ini.c_str()) != 0;  // 0.2.42
        g_sharpGlassKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSharpGlassKey", VK_NUMPAD9, ini.c_str()));
        REX::INFO("Starfield RT: settings bHideGameGlassReflections={} iGlassReflectionKey={:#x} bSharpGlass={} iSharpGlassKey={:#x}",
            g_hideGlassProbe.load(), g_glassReflKey, g_sharpGlass.load(), g_sharpGlassKey);
        g_litTestKey = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iLitTestKey", 0, ini.c_str()));
        g_skipCycleKey = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iSkipPassKey", 0, ini.c_str()));
        rt::EnableDred(GetPrivateProfileIntW(L"Debug", L"bDred", 1, ini.c_str()) != 0);
        rt::timer::SetEnabled(GetPrivateProfileIntW(L"Debug", L"bProfiler", 1, ini.c_str()) != 0);
        REX::INFO("Starfield RT: settings bSoftShadows={} iSunShadowRays={} fSunAngularRadiusDeg={} iSoftShadowMaxBlur={}",
            sun.soft, sun.rays, sun.sunRadiusDeg, sun.maxBlurPx);
        g_baseSun = sun;
        g_sunToggleKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iSunToggleKey", VK_NUMPAD3, ini.c_str()));
        g_probeKey = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iProbeKey", VK_DECIMAL, ini.c_str()));
        g_reflectDebugKey = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iReflectionDebugKey", VK_NUMPAD0, ini.c_str()));
        {
            rt::ReflectionSettings refl;
            wchar_t buf[32]{};
            GetPrivateProfileStringW(L"Debug", L"fReflectionDebugScale", L"1.0", buf, 32, ini.c_str());
            refl.debugScale = static_cast<float>(_wtof(buf));
            refl.maxDistance = static_cast<float>(GetPrivateProfileIntW(L"RT", L"iReflectionRayLength", 3000, ini.c_str()));
            GetPrivateProfileStringW(L"Debug", L"fReflectionSunIntensity", L"1.0", buf, 32, ini.c_str());
            refl.sunIntensity = static_cast<float>(_wtof(buf));
            GetPrivateProfileStringW(L"Debug", L"fReflectionAmbient", L"0.15", buf, 32, ini.c_str());
            refl.ambient = static_cast<float>(_wtof(buf));
            // 0.2.27 (R4)
            refl.startOn = GetPrivateProfileIntW(L"RT", L"iReflections", 1, ini.c_str()) != 0;
            GetPrivateProfileStringW(L"RT", L"fReflectionFadeStart", L"0.45", buf, 32, ini.c_str());
            refl.fadeStart = static_cast<float>(_wtof(buf));
            GetPrivateProfileStringW(L"RT", L"fReflectionFadeEnd", L"0.7", buf, 32, ini.c_str());
            refl.fadeEnd = static_cast<float>(_wtof(buf));
            GetPrivateProfileStringW(L"RT", L"fReflectionBlurPx", L"10", buf, 32, ini.c_str());
            refl.blurPx = static_cast<float>(_wtof(buf));
            GetPrivateProfileStringW(L"RT", L"fReflectionGloss", L"0.65", buf, 32, ini.c_str());  // 0.5.24
            refl.glossRoughScale = (std::clamp)(static_cast<float>(_wtof(buf)), 0.2f, 1.0f);
            GetPrivateProfileStringW(L"RT", L"fReflectionStrength", L"0.5", buf, 32, ini.c_str());
            refl.glossStrength = (std::clamp)(static_cast<float>(_wtof(buf)), 0.5f, 3.0f);
            refl.replaceAll = GetPrivateProfileIntW(L"RT", L"bReplaceGameReflections", 1, ini.c_str()) != 0;
            GetPrivateProfileStringW(L"RT", L"fReflectionProbeAmbient", L"2.5", buf, 32, ini.c_str());
            refl.probeAmbient = static_cast<float>(_wtof(buf));
            GetPrivateProfileStringW(L"RT", L"fReflectionMinWeight", L"0.045", buf, 32, ini.c_str());
            refl.minWeight = static_cast<float>(_wtof(buf));
            refl.traceScale = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"RT", L"iReflectionTraceScale", 2, ini.c_str())), 1, 2);
            refl.hitShadows = GetPrivateProfileIntW(L"RT", L"bReflectionHitShadows", 1, ini.c_str()) != 0;
            refl.smoothGlassTypes = static_cast<unsigned>(GetPrivateProfileIntW(L"RT", L"iSmoothGlassTypes", 4, ini.c_str()));  // 0.2.46
            GetPrivateProfileStringW(L"RT", L"fGlassRoughnessScale", L"0.0", buf, 32, ini.c_str());
            refl.glassRoughScale = static_cast<float>(_wtof(buf));
            refl.thinGlass = GetPrivateProfileIntW(L"RT", L"bThinGlassReflections", 1, ini.c_str()) != 0 ? 1 : 0;  // 0.2.51 (G3)
            refl.thinGlassFallback = GetPrivateProfileIntW(L"RT", L"bThinGlassUntaggedFallback", 0, ini.c_str()) != 0;  // 0.4.10
            GetPrivateProfileStringW(L"RT", L"fThinGlassMinCoverage", L"0.001", buf, 32, ini.c_str());  // 0.2.55: < 0 = no gate
            refl.thinGlassMinCoverage = static_cast<float>(_wtof(buf));
            GetPrivateProfileStringW(L"RT", L"fThinGlassStrength", L"3.0", buf, 32, ini.c_str());
            refl.thinGlassStrength = static_cast<float>(_wtof(buf));
            g_thinGlassKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iThinGlassKey", VK_NUMPAD4, ini.c_str()));
            refl.localLights = GetPrivateProfileIntW(L"RT", L"bReflectionLocalLights", 1, ini.c_str()) != 0;  // 0.2.58
            GetPrivateProfileStringW(L"RT", L"fReflectionLocalLightScale", L"1.0", buf, 32, ini.c_str());
            refl.localLightScale = static_cast<float>(_wtof(buf));
            refl.localShadowRays = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iReflectionLocalShadowRays", 4, ini.c_str()));
            g_localLightKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iLocalLightKey", VK_NUMPAD5, ini.c_str()));
            // 0.3.0: RT diffuse GI
            refl.giMode = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"RT", L"iGI", 1, ini.c_str())), 0, 2);
            GetPrivateProfileStringW(L"RT", L"fGIStrength", L"1.0", buf, 32, ini.c_str());
            refl.giStrength = static_cast<float>(_wtof(buf));
            GetPrivateProfileStringW(L"RT", L"fGIBounce", L"0.5", buf, 32, ini.c_str());
            refl.giBounce = static_cast<float>(_wtof(buf));
            refl.giRayLength = static_cast<float>(GetPrivateProfileIntW(L"RT", L"iGIRayLength", 100, ini.c_str()));
            refl.giHistory = static_cast<float>(GetPrivateProfileIntW(L"RT", L"iGIHistoryFrames", 48, ini.c_str()));
            refl.reflTemporal = GetPrivateProfileIntW(L"RT", L"bReflectionTemporal", 1, ini.c_str()) != 0;  // 0.3.4
            GetPrivateProfileStringW(L"RT", L"fEmissiveScale", L"1.0", buf, 32, ini.c_str());  // 0.3.6
            refl.emissiveScale = static_cast<float>(_wtof(buf));
            refl.emissiveModel = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"RT", L"iEmissiveModel", 0, ini.c_str())), 0, 1);
            GetPrivateProfileStringW(L"RT", L"fGIFilterRadius", L"6.0", buf, 32, ini.c_str());
            refl.giRadius = static_cast<float>(_wtof(buf));
            refl.giTraceScale = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"RT", L"iGITraceScale", 2, ini.c_str())), 1, 2);
            refl.giRays = (std::clamp)(static_cast<int>(GetPrivateProfileIntW(L"RT", L"iGIRays", 1, ini.c_str())), 1, 8);  // 0.4.8
            g_giKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iGIKey", VK_NUMPAD1, ini.c_str()));
            GetPrivateProfileStringW(L"RT", L"fGIScreenLight", L"1.0", buf, 32, ini.c_str());  // 0.3.3
            refl.giScreenLight = static_cast<float>(_wtof(buf));
            g_onScreenMessages = GetPrivateProfileIntW(L"RT", L"bOnScreenMessages", 1, ini.c_str()) != 0;  // 0.3.1
            refl.rtLamps = GetPrivateProfileIntW(L"RT", L"bRTLamps", 1, ini.c_str()) != 0;  // 0.4.0
            refl.hitLampShadowRays = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iHitLampShadowRays", 0, ini.c_str()));  // 0.4.5
            GetPrivateProfileStringW(L"RT", L"fRTLampScale", L"1.0", buf, 32, ini.c_str());
            refl.rtLampScale = static_cast<float>(_wtof(buf));
            g_rtLampKey = static_cast<int>(GetPrivateProfileIntW(L"RT", L"iRTLampKey", VK_NUMPAD2, ini.c_str()));
            g_reflBrighterKey = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iReflectionBrighterKey", VK_ADD, ini.c_str()));
            g_reflDarkerKey = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"iReflectionDarkerKey", VK_SUBTRACT, ini.c_str()));
            REX::INFO("Starfield RT: settings iReflections={} fade {}..{} probe ambient {} sun {} ray length {}", refl.startOn,
                refl.fadeStart, refl.fadeEnd, refl.probeAmbient, refl.sunIntensity, refl.maxDistance);
            g_baseRefl = refl;
        }
        g_iniReflQuality = reflQuality;
        g_iniShadowQuality = shadowQuality;
        g_baselineOff = GetPrivateProfileIntW(L"RT", L"bStartAllOff", 0, ini.c_str()) != 0;  // 0.5.28
        ApplyQuality(reflQuality, shadowQuality, "ini");
        REX::INFO("Starfield RT: settings iSunShadows={} iSunRayLength={} bias {} + {}/unit, iTlasMaxInstanceSize={}, bAlphaTest={}",
            sun.mode, sun.maxDistance, sun.biasBase, sun.biasPerUnit, blas.tlasMaxExtent, blas.alphaTest);
        REX::INFO("Starfield RT: settings bEnableBLAS={} iBlasBuildsPerFrame={} iBlasBudgetMB={} bEnableTLAS={} iTlasMaxInstances={}",
            blas.enabled, blas.buildsPerFrame, blas.budgetMB, blas.tlasEnabled, blas.tlasMaxInstances);
    }

    // ---------------------------------------------------------------- game sun shadow maps (0.1.24)

    // While RT sun shadows are on, shrink the game's directional shadow distance (fMaxShadowDistance:Shadows)
    // so its sun cascades cover nothing and the RT shadow alone decides what is in sunlight. Toggling RT off
    // (F11) restores the original value, so the same key compares RT with the game's own shadows.
    namespace game_shadows
    {
        constexpr std::string_view kKey = "fMaxShadowDistance:Shadows";
        RE::Setting* g_setting = nullptr;
        bool         g_lookedUp = false;
        float        g_original = 0.0f;
        int          g_applied = -1;  // -1 unknown, 0 original value, 1 RT value

        RE::Setting* Find()
        {
            if (auto* ini = RE::INISettingCollection::GetSingleton()) {
                if (auto* s = ini->GetSetting(kKey)) return s;
            }
            if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
                if (auto* s = prefs->GetSetting(kKey)) return s;
            }
            return nullptr;
        }

        void Update(bool rtOn)
        {
            if (!g_replaceGameSunShadows) return;
            if (!g_lookedUp) {
                g_lookedUp = true;
                g_setting = Find();
                if (!g_setting || g_setting->GetType() != RE::Setting::Type::kFloat) {
                    REX::WARN("Starfield RT: {} not found; the game's sun shadow maps stay on", kKey);
                    g_setting = nullptr;
                    return;
                }
                g_original = g_setting->GetValue(0.0f);
                REX::INFO("Starfield RT: {} = {} (original)", kKey, g_original);
            }
            if (!g_setting) return;
            const int want = rtOn ? 1 : 0;
            if (want == g_applied) return;
            g_applied = want;
            g_setting->SetValue(rtOn ? g_gameSunShadowDistanceRT : g_original);
            REX::INFO("Starfield RT: {} -> {} (game sun shadow maps {})", kKey, g_setting->GetValue(0.0f),
                rtOn ? "off, RT shadows replace them" : "restored");
        }
    }

    // ---------------------------------------------------------------- utilities

    std::uintptr_t ModuleBase() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

    std::string ModuleNameOf(const void* addr)
    {
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(addr), &mod)) {
            return "?";
        }
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, MAX_PATH);
        std::string name = path;
        if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        return name;
    }

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

    std::string StatesToString(D3D12_RESOURCE_STATES s)
    {
        if (s == D3D12_RESOURCE_STATE_COMMON) return "COMMON";
        std::string out;
        auto add = [&](D3D12_RESOURCE_STATES bit, const char* n) { if ((s & bit) == bit && bit) { out += out.empty() ? "" : "|"; out += n; } };
        add(D3D12_RESOURCE_STATE_RENDER_TARGET, "RT");
        add(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "UAV");
        add(D3D12_RESOURCE_STATE_DEPTH_WRITE, "DEPTH_W");
        add(D3D12_RESOURCE_STATE_DEPTH_READ, "DEPTH_R");
        add(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "SRV_NPS");
        add(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "SRV_PS");
        add(D3D12_RESOURCE_STATE_COPY_DEST, "COPY_D");
        add(D3D12_RESOURCE_STATE_COPY_SOURCE, "COPY_S");
        add(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, "INDIRECT");
        add(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "VB_CB");
        add(D3D12_RESOURCE_STATE_INDEX_BUFFER, "IB");
        add(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "RTAS");
        return out.empty() ? std::format("{:#x}", static_cast<unsigned>(s)) : out;
    }

    std::string DescribeResource(ID3D12Resource* r)
    {
        if (!r) return "null";
        const auto d = r->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            return std::format("buffer {}B", d.Width);
        }
        return std::format("tex{} {}x{}{} fmt{} mips{}", d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? "3D" : "2D",
            d.Width, d.Height, d.DepthOrArraySize > 1 ? std::format("x{}", d.DepthOrArraySize) : "",
            static_cast<int>(d.Format), d.MipLevels);
    }

    // 0.2.43: describe a table entry from the shape recorded when its view was written. Never calls into the texture:
    // unused table slots can hold stale entries for textures the game has freed (calling GetDesc on one crashed F8).
    std::string DescribeView(const rt::BufferView& v)
    {
        return std::format("tex{} {}x{}{} fmt{} mips{}", v.texDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? "3D" : "2D", v.texWidth,
            v.texHeight, v.texDepthOrArray > 1 ? std::format("x{}", v.texDepthOrArray) : "", static_cast<int>(v.texFormat), v.texMips);
    }

    // ---------------------------------------------------------------- per-thread observation

    struct BarrierRecord
    {
        ID3D12Resource*       resource = nullptr;
        D3D12_RESOURCE_STATES before{}, after{};
        bool                  uav = false;
    };

    struct PassObservation
    {
        int                        pass = -1;
        ID3D12GraphicsCommandList* list = nullptr;
        std::uint32_t              draws = 0, dispatches = 0;
        bool                       indexed = false;
        UINT                       drawCount = 0, drawInstances = 0;
        UINT                       dispX = 0, dispY = 0, dispZ = 0;
        std::array<BarrierRecord, kMaxBarriersLogged> barriers{};
        std::uint32_t              barrierCount = 0;
    };

    // Render-target/viewport state of the list this thread records, tracked all the time
    // (the render graph binds targets before calling a pass's Execute).
    constexpr std::size_t kTrackedRootParams = 8;
    constexpr std::size_t kTrackedRootConstants = 64;

    struct ListState
    {
        ID3D12GraphicsCommandList*  list = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        std::uint32_t               rtvCount = 0;
        bool                        hasDsv = false;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};          // 0.2.10: bound depth-stencil view
        D3D12_VIEWPORT              viewport{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvs{};  // 0.2.22: every bound render target

        // 0.2.22: graphics-side bindings (reflection survey).
        ID3D12RootSignature*        graphicsRootSig = nullptr;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 16> graphicsTables{};
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, 16>   graphicsRootCbv{};

        // Compute-side bindings, so injected work can restore exactly what the engine had set.
        ID3D12RootSignature*        computeRootSig = nullptr;
        ID3D12PipelineState*        pso = nullptr;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kTrackedRootParams> computeTables{};
        std::array<std::array<UINT, kTrackedRootConstants>, kTrackedRootParams> computeConsts{};
        std::array<UINT, kTrackedRootParams> computeConstCount{};
        // 0.4.3: root descriptors (CBV / SRV / UAV by address): kind 0 = unset, 1 CBV, 2 SRV, 3 UAV.
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, kTrackedRootParams> computeRootVa{};
        std::array<std::uint8_t, kTrackedRootParams> computeRootKind{};
        UINT                        lastDispatchX = 0, lastDispatchY = 0, lastDispatchZ = 0;
    };

    // 0.2.10: depth clear values the game uses per DSV (learned from ClearDepthStencilView), and the DSVs the
    // sun shadow pass binds, so the plugin can re-clear the sun cascades after the pass instead of dropping draws.
    std::mutex                                   g_dsvClearLock;
    std::unordered_map<std::size_t, float>       g_dsvClearValue;
    thread_local bool                            t_collectDsvs = false;
    thread_local std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> t_passDsvs;
    void NoteDsv(const D3D12_CPU_DESCRIPTOR_HANDLE& h)
    {
        if (!t_collectDsvs || !h.ptr) return;
        for (const auto& d : t_passDsvs) if (d.ptr == h.ptr) return;
        t_passDsvs.push_back(h);
    }

    thread_local PassObservation* t_obs = nullptr;  // innermost pass being executed on this thread
    thread_local ListState        t_state;
    thread_local bool             t_injecting = false;  // our own calls: don't track them

    ListState& StateFor(ID3D12GraphicsCommandList* list)
    {
        if (t_state.list != list) {
            t_state = {};
            t_state.list = list;
        }
        return t_state;
    }

    PassObservation* Obs(ID3D12GraphicsCommandList* list)
    {
        if (t_obs) {
            t_obs->list = list;
        }
        return t_obs;
    }

    // ---------------------------------------------------------------- command-list hooks

    enum : std::size_t
    {
        kDrawInstanced = 12,
        kDrawIndexedInstanced = 13,
        kDispatch = 14,
        kRSSetViewports = 21,
        kSetPipelineState = 25,
        kResourceBarrier = 26,
        kSetComputeRootSignature = 29,
        kSetGraphicsRootSignature = 30,
        kSetGraphicsRootDescriptorTable = 32,
        kSetGraphicsRootConstantBufferView = 38,
        kSetComputeRootDescriptorTable = 31,
        kSetComputeRoot32BitConstant = 33,
        kSetComputeRoot32BitConstants = 35,
        kSetComputeRootConstantBufferView = 37,   // 0.4.3
        kSetComputeRootShaderResourceView = 39,
        kSetComputeRootUnorderedAccessView = 41,
        kOMSetRenderTargets = 46,
        kClearDepthStencilView = 47,
        kExecuteIndirect = 59,
        kBeginRenderPass = 68,
        kVtableSlots = 70,
    };

    std::array<void*, kVtableSlots> g_origListFn{};
    template <class Fn>
    Fn Orig(std::size_t index) { return reinterpret_cast<Fn>(g_origListFn[index]); }

    using L = ID3D12GraphicsCommandList;

    // ---------------------------------------------------------------- 0.2.22: R1 reflection survey
    // While GatherIndirect executes, each draw logs its render targets, viewport and every graphics root binding
    // (descriptor tables resolved entry by entry). Runs on the first in-game frame and again on every F8 press.
    int                g_gatherIndirectPass = -1;
    // 0.2.38 (G1): glass survey window. Armed for the next 3 frames; each glass pass logs its first 2 executions
    // in the window, with up to 6 draws each.
    std::atomic<std::uint64_t> g_glassSurveyUntil{ 0 };
    std::atomic<int>           g_glassSurveyId{ 0 };
    std::array<std::atomic<int>, 64> g_glassSurveyCount{};
    std::array<std::atomic<int>, 64> g_glassSurveySeen{};

    // 0.2.39 (G2): hide the game's probe reflection on glass. Glass is drawn by MomentBasedOITRenderTransparency;
    // its root table 4 entry +7 is the prefiltered reflection-probe cube array (the same one GatherIndirect reads).
    // Each pass gets its own table-4 block per frame, so while that pass records, the plugin rewrites +7 of the
    // table it binds with a black cube array of the same shape: only the glass draws lose the probe reflection.
    int               g_oitTransparencyPass = -1;
    int               g_oitCompositePass = -1;     // 0.2.51 (G3): CompositeMomentBasedOIT, thin-glass reflections after it
    thread_local bool t_inOitTransparency = false;
    std::mutex        g_blackCubeLock;
    // 0.2.40: one black texture per (format, cube array or single cube). Some glass materials (the thick doors) bind
    // their own static environment cube (TextureCube) at +7 instead of the probe array.
    struct BlackCube
    {
        ComPtr<ID3D12Resource> res;
        UINT                   slices = 0;
        DXGI_FORMAT            format = DXGI_FORMAT_UNKNOWN;
        bool                   array = false;
    };
    std::vector<BlackCube> g_blackCubes;
    std::atomic<int>  g_glassSwapLogged{ 0 };
    std::atomic<std::uint64_t> g_glassSwaps{ 0 };

    ID3D12Device* GetEngineDevice();  // defined further down

    void HideGlassProbe(D3D12_GPU_DESCRIPTOR_HANDLE table)
    {
        constexpr std::uint32_t kProbeEntry = 7;
        rt::BufferView v{};
        const bool found = rt::LookupGpuTableAny(table, kProbeEntry, v) && v.texture;
        const bool isArray = found && v.viewDim == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        const bool isCube = found && v.viewDim == D3D12_SRV_DIMENSION_TEXTURECUBE;
        if (!isArray && !isCube) {
            if (g_glassSwapLogged.fetch_add(1) < 4) {
                REX::WARN("Starfield RT: [G2] glass table {:#x} +7 is not a cube (texture {:p}, viewDim {}); left alone",
                    table.ptr, static_cast<void*>(v.texture), v.viewDim);
            }
            return;
        }
        auto* device = GetEngineDevice();
        if (!device) return;
        std::lock_guard lock(g_blackCubeLock);
        for (const auto& b : g_blackCubes) {
            if (b.res.Get() == v.texture) return;  // already swapped (table reused within the frame)
        }
        const UINT slices = isArray ? v.texDepthOrArray : 6u;  // 0.2.43: recorded shape, no call into the texture
        if (slices < 6) return;
        BlackCube* black = nullptr;
        for (auto& b : g_blackCubes) {
            if (b.format == v.format && b.array == isArray && b.slices >= slices) { black = &b; break; }
        }
        if (!black) {
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = 1;
            d.Height = 1;
            d.DepthOrArraySize = static_cast<UINT16>(slices);
            d.MipLevels = 1;
            d.Format = v.format;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            BlackCube b;  // committed resources start zeroed: black
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&b.res)))) {
                REX::WARN("Starfield RT: [G2] black cube ({} slices, format {}) could not be created", slices, static_cast<int>(v.format));
                return;
            }
            b.res->SetName(L"StarfieldRT black cube");
            b.slices = slices;
            b.format = v.format;
            b.array = isArray;
            g_blackCubes.push_back(std::move(b));  // kept for the session (a few bytes each)
            black = &g_blackCubes.back();
            REX::INFO("Starfield RT: [G2] black {} created ({} slices, format {})", isArray ? "cube array" : "cube", slices,
                static_cast<int>(v.format));
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        if (!rt::GpuTableToCpu(table, kProbeEntry, cpu)) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = v.format;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (isArray) {
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            sd.TextureCubeArray.MipLevels = 1;
            sd.TextureCubeArray.NumCubes = slices / 6;
        } else {
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = 1;
        }
        device->CreateShaderResourceView(black->res.Get(), &sd, cpu);
        const auto n = g_glassSwaps.fetch_add(1);
        if (n < 2 || n % 3600 == 0) {
            REX::INFO("Starfield RT: [G2] glass probe reflection hidden (table {:#x}, heap index {}, {}, {} swaps so far)", table.ptr,
                rt::HeapIndexOf(table), isArray ? "probe array" : "material cube", n + 1);
        }
    }
    std::atomic<bool>  g_indirectSurveyRequest{ false };
    std::atomic<bool>  g_indirectSurveyDone{ false };
    thread_local bool  t_inGatherIndirect = false;
    thread_local int   t_surveyDraws = -1;  // draws left to log in this execution (-1 = not surveying)

    // 0.2.38 (G1): the same per-draw dump for the glass passes, tagged with the pass name.
    thread_local const char* t_glassPassName = nullptr;
    thread_local int         t_glassDraws = -1;

    void SurveyDraw(L* l, const char* kind, UINT count, UINT instances, const char* tag = "[R1] GatherIndirect")
    {
        const auto& s = StateFor(l);
        REX::INFO("Starfield RT: {} {} {}x{} on list {:p}: {} RT(s), viewport {}x{}, DSV {}, pso {:p}", tag, kind, count, instances,
            static_cast<void*>(l), s.rtvCount, s.viewport.Width, s.viewport.Height, s.hasDsv, static_cast<void*>(s.pso));
        for (std::uint32_t i = 0; i < s.rtvCount && i < s.rtvs.size(); ++i) {
            ID3D12Resource* res = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            if (rt::LookupRtv(s.rtvs[i].ptr, res, fmt)) {
                REX::INFO("Starfield RT: [R1]   RT{}: resource {:p} view fmt {} ({})", i, static_cast<void*>(res), static_cast<int>(fmt),
                    DescribeResource(res));
            } else {
                REX::INFO("Starfield RT: [R1]   RT{}: handle {:#x} not tracked", i, s.rtvs[i].ptr);
            }
        }
        REX::INFO("Starfield RT: [R1]   graphics root signature {:p}", static_cast<void*>(s.graphicsRootSig));
        for (std::size_t p = 0; p < s.graphicsRootCbv.size(); ++p) {
            if (s.graphicsRootCbv[p]) REX::INFO("Starfield RT: [R1]   root param {}: root CBV {:#x}", p, s.graphicsRootCbv[p]);
        }
        for (std::size_t p = 0; p < s.graphicsTables.size(); ++p) {
            const auto t = s.graphicsTables[p];
            if (!t.ptr) continue;
            REX::INFO("Starfield RT: [R1]   root param {}: table {:#x} (heap index {})", p, t.ptr, rt::HeapIndexOf(t));
            int blank = 0;
            for (std::uint32_t k = 0; k < 40 && blank < 6; ++k) {
                rt::BufferView v{};
                if (!rt::LookupGpuTableAny(t, k, v)) {
                    ++blank;
                    continue;
                }
                blank = 0;
                if (v.texture) {
                    REX::INFO("Starfield RT: [R1]     +{}: {} texture {:p} view fmt {} viewDim {} ({})", k, v.uav ? "UAV" : "SRV",
                        static_cast<void*>(v.texture), static_cast<int>(v.format), v.viewDim, DescribeView(v));
                } else if (v.cbv) {
                    REX::INFO("Starfield RT: [R1]     +{}: CBV {:#x} {} B", k, v.gpuva, v.sizeBytes);
                } else {
                    REX::INFO("Starfield RT: [R1]     +{}: {} buffer {:p} {} B stride {}{}", k, v.uav ? "UAV" : "SRV",
                        static_cast<void*>(v.resource), v.sizeBytes, v.stride, v.raw ? " raw" : "");
                }
            }
        }
    }

    // 0.2.42: the view through glass. The glass draws read the scene behind them from a mip chain of the scene colour
    // (table 4 +15, Texture2D with 6 mips); rough or frosted glass samples the blurrier mips. Rewriting that entry as
    // a view of mip 0 only makes every glass draw see the sharp image (frosted glass turns clear too).
    std::atomic<int>           g_sharpLogged{ 0 };
    std::atomic<std::uint64_t> g_sharpSwaps{ 0 };

    void SharpenGlassView(D3D12_GPU_DESCRIPTOR_HANDLE table)
    {
        constexpr std::uint32_t kSceneEntry = 15;
        rt::BufferView v{};
        if (!rt::LookupGpuTableAny(table, kSceneEntry, v) || !v.texture || v.viewDim != D3D12_SRV_DIMENSION_TEXTURE2D) {
            if (g_sharpLogged.fetch_add(1) < 4) {
                REX::WARN("Starfield RT: [glass] table {:#x} +15 is not a 2D texture (texture {:p}, viewDim {}); left alone", table.ptr,
                    static_cast<void*>(v.texture), v.viewDim);
            }
            return;
        }
        if (v.texMips <= 1 || v.texWidth < 640 || v.texDepthOrArray != 1) return;  // not the scene colour mip chain (0.2.43: recorded shape)
        auto* device = GetEngineDevice();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        if (!device || !rt::GpuTableToCpu(table, kSceneEntry, cpu)) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = v.format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels = 1;  // blurrier mips clamp to the sharp one
        device->CreateShaderResourceView(v.texture, &sd, cpu);
        const auto n = g_sharpSwaps.fetch_add(1);
        if (n < 2 || n % 3600 == 0) {
            REX::INFO("Starfield RT: [glass] view through glass sharpened ({}x{}, {} mips -> 1; {} so far)", v.texWidth, v.texHeight, v.texMips, n + 1);
        }
    }

    void STDMETHODCALLTYPE H_DrawInstanced(L* l, UINT a, UINT b, UINT c, UINT d)
    {
        if (t_inGatherIndirect && t_surveyDraws > 0) {
            --t_surveyDraws;
            SurveyDraw(l, "DrawInstanced", a, b);
        }
        if (t_glassDraws > 0) {
            --t_glassDraws;
            SurveyDraw(l, "DrawInstanced", a, b, t_glassPassName);
        }
        if (t_suppressDraws) return;  // 0.2.5 skip test
        if (auto* o = Obs(l); o && o->draws++ == 0) {
            o->indexed = false; o->drawCount = a; o->drawInstances = b;
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, UINT, UINT, UINT)>(kDrawInstanced)(l, a, b, c, d);
    }
    void STDMETHODCALLTYPE H_DrawIndexedInstanced(L* l, UINT a, UINT b, UINT c, INT d, UINT e)
    {
        if (t_inGatherIndirect && t_surveyDraws > 0) {
            --t_surveyDraws;
            SurveyDraw(l, "DrawIndexedInstanced", a, b);
        }
        if (t_glassDraws > 0) {
            --t_glassDraws;
            SurveyDraw(l, "DrawIndexedInstanced", a, b, t_glassPassName);
        }
        if (t_suppressDraws) return;  // 0.2.5 skip test
        if (auto* o = Obs(l); o && o->draws++ == 0) {
            o->indexed = true; o->drawCount = a; o->drawInstances = b;
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, UINT, UINT, INT, UINT)>(kDrawIndexedInstanced)(l, a, b, c, d, e);
    }
    // 0.2.10: remember the depth value each DSV is cleared with (the sun cascades are re-cleared with it).
    void STDMETHODCALLTYPE H_ClearDepthStencilView(L* l, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
        UINT8 stencil, UINT rects, const D3D12_RECT* r)
    {
        if ((flags & D3D12_CLEAR_FLAG_DEPTH) && rects == 0 && dsv.ptr) {
            std::lock_guard lock(g_dsvClearLock);
            g_dsvClearValue[dsv.ptr] = depth;
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*)>(
            kClearDepthStencilView)(l, dsv, flags, depth, stencil, rects, r);
    }

    // 0.2.5: ExecuteIndirect (shadow maps are drawn this way). Counted as a draw in the pass survey.
    void STDMETHODCALLTYPE H_ExecuteIndirect(L* l, ID3D12CommandSignature* sig, UINT maxCount, ID3D12Resource* args, UINT64 argOffset,
        ID3D12Resource* count, UINT64 countOffset)
    {
        // 0.2.48: thick glass (the doors) is drawn twice: into the G-buffer (where the RT reflection lands) and again in
        // the transparency pass, which refracts a copy of the scene that already shows the door's reflection: a warped
        // second reflection on top. Those transparency draws bind their material's own TextureCube at table 4 +7 (thin
        // glass binds the probe cube array), so they can be told apart and skipped.
        if (t_inOitTransparency && g_skipThickGlassLayer.load(std::memory_order_relaxed)) {
            const auto t4 = StateFor(l).graphicsTables[4];
            rt::BufferView v{};
            if (t4.ptr && rt::LookupGpuTableAny(t4, 7, v) && v.texture && v.viewDim == D3D12_SRV_DIMENSION_TEXTURECUBE) {
                static std::atomic<std::uint64_t> skipped{ 0 };
                const auto n = skipped.fetch_add(1);
                if (n < 2 || n % 3600 == 0) {
                    REX::INFO("Starfield RT: thick-glass see-through layer skipped ({} draws in this batch, {} batches so far)", maxCount, n + 1);
                }
                return;
            }
        }
        if (t_glassDraws > 0) {
            --t_glassDraws;
            SurveyDraw(l, "ExecuteIndirect", maxCount, 0, t_glassPassName);
            REX::INFO("Starfield RT: {}   command signature {:p}, args {:p} +{}, count buffer {:p}", t_glassPassName,
                static_cast<void*>(sig), static_cast<void*>(args), argOffset, static_cast<void*>(count));
        }
        if (t_suppressDraws) return;
        if (auto* o = Obs(l); o && o->draws++ == 0) {
            o->indexed = true; o->drawCount = maxCount; o->drawInstances = 0;
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64)>(kExecuteIndirect)(
            l, sig, maxCount, args, argOffset, count, countOffset);
    }
    void STDMETHODCALLTYPE H_Dispatch(L* l, UINT x, UINT y, UINT z)
    {
        if (!t_injecting) {
            if (auto* o = Obs(l); o && o->dispatches++ == 0) {
                o->dispX = x; o->dispY = y; o->dispZ = z;
            }
            auto& s = StateFor(l);
            s.lastDispatchX = x; s.lastDispatchY = y; s.lastDispatchZ = z;
            ++t_dispatchCount;
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, UINT, UINT)>(kDispatch)(l, x, y, z);
    }
    void STDMETHODCALLTYPE H_SetPipelineState(L* l, ID3D12PipelineState* p)
    {
        if (!t_injecting) StateFor(l).pso = p;
        Orig<void(STDMETHODCALLTYPE*)(L*, ID3D12PipelineState*)>(kSetPipelineState)(l, p);
    }
    void STDMETHODCALLTYPE H_SetComputeRootSignature(L* l, ID3D12RootSignature* r)
    {
        if (!t_injecting) {
            auto& s = StateFor(l);
            if (s.computeRootSig != r) {  // a new root signature invalidates all root bindings
                s.computeTables = {};
                s.computeConstCount = {};
                s.computeRootKind = {};
            }
            s.computeRootSig = r;
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, ID3D12RootSignature*)>(kSetComputeRootSignature)(l, r);
    }
    void STDMETHODCALLTYPE H_SetGraphicsRootSignature(L* l, ID3D12RootSignature* r)
    {
        if (!t_injecting) {
            auto& s = StateFor(l);
            if (s.graphicsRootSig != r) {
                s.graphicsTables = {};
                s.graphicsRootCbv = {};
            }
            s.graphicsRootSig = r;
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, ID3D12RootSignature*)>(kSetGraphicsRootSignature)(l, r);
    }
    // 0.2.54: every distinct root-table-1 handle the game binds (compute or graphics), newest last. The scene table moves
    // to a new heap slot every frame, and at some locations the RT build pass binds something else in slot 1, so the
    // build picks the newest handle here that currently holds the scene table.
    constexpr std::uint32_t kTable1Ring = 64;
    std::array<std::atomic<std::uint64_t>, kTable1Ring> g_table1Ring{};
    std::atomic<std::uint32_t> g_table1Pos{ 0 };
    inline void NoteTable1(std::uint64_t h)
    {
        thread_local std::uint64_t last = 0;
        if (!h || h == last) return;
        last = h;
        g_table1Ring[g_table1Pos.fetch_add(1) % kTable1Ring].store(h, std::memory_order_relaxed);
    }

    // 0.3.1: hotkey feedback. Logs the line and shows it as a HUD message (the game's own top-left notification), sent
    // from the game's main thread through the SFSE task queue (hotkeys are polled on a render thread).
    void Announce(std::string_view what, std::string_view state)
    {
        REX::INFO("Starfield RT: {} -> {}", what, state);
        if (!g_onScreenMessages) return;
        const auto* tasks = SFSE::GetTaskInterface();
        if (!tasks) return;
        std::string text = std::format("Starfield RT - {}: {}", what, state);
        tasks->AddTask([text = std::move(text)]() {
            // Same layout as CommonLibSF's RE::ShowHUDMessageEvent (0x18 B); its header doesn't compile on its own here.
            struct HudMessage { RE::BSFixedString text, sound; bool canThrottle, isWarning; };
            static_assert(sizeof(HudMessage) == 0x18);
            using GetSourceFn = RE::BSTEventSource<HudMessage>* (*)();
            static REL::Relocation<GetSourceFn> getSource{ REL::ID(86800) };  // ShowHUDMessageEvent::GetEventSource
            if (auto* source = getSource()) {
                HudMessage ev{ RE::BSFixedString(text.c_str()), RE::BSFixedString(""), false, false };
                source->Notify(ev);
            }
        });
    }

    void STDMETHODCALLTYPE H_SetGraphicsRootDescriptorTable(L* l, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h)
    {
        if (!t_injecting && index < 16) StateFor(l).graphicsTables[index] = h;
        if (!t_injecting && index == 1) NoteTable1(h.ptr);  // 0.2.54
        if (t_inOitTransparency && index == 4 && h.ptr) {
            if (g_hideGlassProbe.load(std::memory_order_relaxed)) HideGlassProbe(h);  // 0.2.39 (G2)
            if (g_sharpGlass.load(std::memory_order_relaxed)) SharpenGlassView(h);   // 0.2.42
            // 0.5.23: the same scene-colour texture (table 4 + 15, a mip chain of the lit image before transparencies)
            // goes to DLSS Ray Reconstruction as its colour-before-transparency guide.
            rt::BufferView sv{};
            if (rt::LookupGpuTableAny(h, 15, sv) && sv.texture && sv.viewDim == D3D12_SRV_DIMENSION_TEXTURE2D && sv.texMips > 1 &&
                sv.texWidth >= 640 && sv.texDepthOrArray == 1) {
                rt::NoteColorBeforeTransparency(sv.texture);
            }
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(kSetGraphicsRootDescriptorTable)(l, index, h);
    }
    void STDMETHODCALLTYPE H_SetGraphicsRootConstantBufferView(L* l, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va)
    {
        if (!t_injecting && index < 16) StateFor(l).graphicsRootCbv[index] = va;
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(kSetGraphicsRootConstantBufferView)(l, index, va);
    }
    // 0.2.58: the main view's tiled lighting (TiledDeferredLight, PSO 11798/11809) binds a compute root table 4 holding its
    // local-light constant buffers (cb0/cb1/cb2 space4 = 64 KB shadow data, 40 KB, 64 KB light records) and the light
    // index ranges (t0 space4, one 48-byte struct). Recognised by the 40 KB CBV; the light records are the 64 KB CBV right
    // after it. A render-resolution UAV texture in the same table rules out the probe (cubemap) lighting views.
    ID3D12Device* GetEngineDevice();
    void RestoreComputeState(ID3D12GraphicsCommandList* list, const ListState& s);  // defined further down
    std::uint64_t CurrentFrame();  // 0.5.36: defined after g_frameCounter
    void NoteLightTable(L* list, D3D12_GPU_DESCRIPTOR_HANDLE h)
    {
        if (!h.ptr) return;
        // 0.2.60: fast path with the layout seen in 0.2.58's log: +0 ranges (48 B), +1 64 KB, +2 40 KB, +3 64 KB light
        // records, +15 the render-resolution UAV. Checked on every bind (a thread can bind the same table handle again in a
        // later frame with new contents, which 0.2.58 skipped: the lights then blinked off).
        {
            rt::BufferView c0{}, c2{}, c3{}, u{};
            if (rt::LookupGpuTableAny(h, 2, c2) && c2.cbv && c2.sizeBytes == 40960) {
                if (rt::LookupGpuTableAny(h, 3, c3) && c3.cbv && c3.sizeBytes == 65536 && rt::LookupGpuTableAny(h, 0, c0) &&
                    !c0.cbv && c0.resource && c0.stride == 48 && rt::LookupGpuTableAny(h, 15, u) && u.uav && u.texture &&
                    u.texWidth >= 640) {
                    const bool prev = t_injecting;
                    t_injecting = true;
                    bool switchOff = false;
                    auto* device = GetEngineDevice();
                    ID3D12Resource* slot = rt::CaptureLocalLights(device, list, c3.gpuva, c0.gpuva, h, switchOff);
                    {
                        // 0.5.36: lamp flicker outdoors (Jemison) with RT lamps on. Log every lamp-list capture for a while
                        // after RT lamps are switched on: how many per frame, from which source, and whether the game's
                        // lamps were switched off for that table.
                        static std::atomic<int> logged{ 0 };
                        static std::atomic<std::uint64_t> lastFrame{ 0 };
                        static std::atomic<int> perFrame{ 0 };
                        const std::uint64_t f = CurrentFrame();
                        if (lastFrame.exchange(f) != f) perFrame = 0;
                        const int nth = perFrame.fetch_add(1) + 1;
                        if (rt::DirectLightsMode() != 0 && logged.fetch_add(1) < 120) {
                            REX::INFO("Starfield RT: [lamps] capture frame {} #{}: records {:#x}, ranges {:#x}, table {:#x}, list {:p}, thread {}, game lamps {}",
                                f, nth, c3.gpuva, c0.gpuva, h.ptr, static_cast<void*>(list), GetCurrentThreadId(),
                                slot && switchOff ? "switched off" : "left on");
                        }
                    }
                    RestoreComputeState(list, StateFor(list));  // 0.4.3: the GPU copy changed the compute state
                    if (slot && switchOff && device) {
                        // 0.4.3: the game's lighting reads its lamp ranges through this table: point that entry at our
                        // copy with the point/box/spot ranges emptied, so its tiled lighting skips the lamps we draw.
                        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
                        if (rt::GpuTableToCpu(h, 0, cpu)) {
                            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                            sd.Format = DXGI_FORMAT_UNKNOWN;
                            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            sd.Buffer.FirstElement = 65760 / 48;
                            sd.Buffer.NumElements = 1;
                            sd.Buffer.StructureByteStride = 48;
                            device->CreateShaderResourceView(slot, &sd, cpu);
                        }
                    }
                    t_injecting = prev;
                    return;
                }
            } else {
                return;  // not the lighting table (most binds end here after one lookup)
            }
        }
        thread_local std::uint64_t last = 0;  // only the diagnostic scan below is deduplicated
        if (h.ptr == last) return;
        last = h.ptr;
        constexpr std::uint32_t kScan = 24;
        rt::BufferView v[kScan]{};
        bool have[kScan]{};
        int k40 = -1, counts = -1;
        std::uint32_t uavWidth = 0;
        for (std::uint32_t k = 0; k < kScan; ++k) {
            have[k] = rt::LookupGpuTableAny(h, k, v[k]);
            if (!have[k]) continue;
            if (v[k].cbv && v[k].sizeBytes == 40960 && k40 < 0) k40 = static_cast<int>(k);
            if (!v[k].cbv && v[k].resource && v[k].stride == 48 && v[k].sizeBytes <= 48 * 4 && counts < 0) counts = static_cast<int>(k);
            if (v[k].uav && v[k].texture) uavWidth = (std::max)(uavWidth, v[k].texWidth);
        }
        if (k40 < 0) return;
        int lightsAt = -1;
        if (k40 + 1 < static_cast<int>(kScan) && have[k40 + 1] && v[k40 + 1].cbv && v[k40 + 1].sizeBytes == 65536) lightsAt = k40 + 1;
        static std::atomic<int> logged{ 0 };
        if (logged.fetch_add(1) < 2) {
            std::string s;
            for (std::uint32_t k = 0; k < kScan; ++k) {
                if (!have[k]) continue;
                s += std::format(" +{}:{}{}", k, v[k].cbv ? "CBV" : v[k].uav ? "UAV" : "SRV",
                    v[k].texture ? std::format("(tex {}x{})", v[k].texWidth, v[k].texHeight)
                                 : std::format("({} B, stride {})", v[k].sizeBytes, v[k].stride));
            }
            REX::INFO("Starfield RT: [L] light table seen (lights at +{}, ranges at +{}, UAV width {}):{}", lightsAt, counts, uavWidth, s);
        }
    }

    void STDMETHODCALLTYPE H_SetComputeRootDescriptorTable(L* l, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h)
    {
        if (!t_injecting && index < kTrackedRootParams) StateFor(l).computeTables[index] = h;
        if (!t_injecting && index == 1) NoteTable1(h.ptr);
        if (!t_injecting && index == 4) NoteLightTable(l, h);  // 0.2.58
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(kSetComputeRootDescriptorTable)(l, index, h);
    }
    // 0.4.3: compute root descriptors, tracked so injected work can restore them.
    void NoteComputeRootVa(L* l, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va, std::uint8_t kind)
    {
        if (t_injecting || index >= kTrackedRootParams) return;
        auto& s = StateFor(l);
        s.computeRootVa[index] = va;
        s.computeRootKind[index] = kind;
    }
    void STDMETHODCALLTYPE H_SetComputeRootCbv(L* l, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va)
    {
        NoteComputeRootVa(l, index, va, 1);
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(kSetComputeRootConstantBufferView)(l, index, va);
    }
    void STDMETHODCALLTYPE H_SetComputeRootSrv(L* l, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va)
    {
        NoteComputeRootVa(l, index, va, 2);
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(kSetComputeRootShaderResourceView)(l, index, va);
    }
    void STDMETHODCALLTYPE H_SetComputeRootUav(L* l, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va)
    {
        NoteComputeRootVa(l, index, va, 3);
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(kSetComputeRootUnorderedAccessView)(l, index, va);
    }
    void STDMETHODCALLTYPE H_SetComputeRoot32BitConstant(L* l, UINT index, UINT value, UINT offset)
    {
        if (!t_injecting && index < kTrackedRootParams && offset < kTrackedRootConstants) {
            auto& s = StateFor(l);
            s.computeConsts[index][offset] = value;
            s.computeConstCount[index] = (std::max)(s.computeConstCount[index], offset + 1);
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, UINT, UINT)>(kSetComputeRoot32BitConstant)(l, index, value, offset);
    }
    void STDMETHODCALLTYPE H_SetComputeRoot32BitConstants(L* l, UINT index, UINT count, const void* data, UINT offset)
    {
        if (!t_injecting && index < kTrackedRootParams && data && offset + count <= kTrackedRootConstants) {
            auto& s = StateFor(l);
            std::memcpy(&s.computeConsts[index][offset], data, count * sizeof(UINT));
            s.computeConstCount[index] = (std::max)(s.computeConstCount[index], offset + count);
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, UINT, const void*, UINT)>(kSetComputeRoot32BitConstants)(l, index, count, data, offset);
    }
    void STDMETHODCALLTYPE H_RSSetViewports(L* l, UINT n, const D3D12_VIEWPORT* v)
    {
        if (n > 0 && v) StateFor(l).viewport = v[0];
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, const D3D12_VIEWPORT*)>(kRSSetViewports)(l, n, v);
    }
    void STDMETHODCALLTYPE H_ResourceBarrier(L* l, UINT n, const D3D12_RESOURCE_BARRIER* b)
    {
        if (auto* o = Obs(l); o && b) {
            for (UINT i = 0; i < n && o->barrierCount < kMaxBarriersLogged; ++i) {
                auto& rec = o->barriers[o->barrierCount];
                if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                    rec = { b[i].Transition.pResource, b[i].Transition.StateBefore, b[i].Transition.StateAfter, false };
                    ++o->barrierCount;
                } else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                    rec = { b[i].UAV.pResource, {}, {}, true };
                    ++o->barrierCount;
                }
            }
        }
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, const D3D12_RESOURCE_BARRIER*)>(kResourceBarrier)(l, n, b);
    }
    void STDMETHODCALLTYPE H_OMSetRenderTargets(L* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single,
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
    {
        auto& s = StateFor(l);
        s.rtvCount = n;
        s.rtv = (n > 0 && rts) ? rts[0] : D3D12_CPU_DESCRIPTOR_HANDLE{};
        s.rtvs = {};
        for (UINT i = 0; rts && i < n && i < s.rtvs.size(); ++i) {
            s.rtvs[i] = single ? D3D12_CPU_DESCRIPTOR_HANDLE{ rts[0].ptr + SIZE_T(i) * rt::RtvIncrement() } : rts[i];
        }
        s.hasDsv = dsv != nullptr;
        s.dsv = dsv ? *dsv : D3D12_CPU_DESCRIPTOR_HANDLE{};
        if (dsv) NoteDsv(*dsv);
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*)>(
            kOMSetRenderTargets)(l, n, rts, single, dsv);
    }
    void STDMETHODCALLTYPE H_BeginRenderPass(L* l, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS f)
    {
        auto& s = StateFor(l);
        s.rtvCount = n;
        s.rtv = (n > 0 && rts) ? rts[0].cpuDescriptor : D3D12_CPU_DESCRIPTOR_HANDLE{};
        s.rtvs = {};
        for (UINT i = 0; rts && i < n && i < s.rtvs.size(); ++i) s.rtvs[i] = rts[i].cpuDescriptor;
        s.hasDsv = ds != nullptr;
        s.dsv = ds ? ds->cpuDescriptor : D3D12_CPU_DESCRIPTOR_HANDLE{};
        if (ds) NoteDsv(ds->cpuDescriptor);
        Orig<void(STDMETHODCALLTYPE*)(L*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
            const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS)>(kBeginRenderPass)(l, n, rts, ds, f);
    }

    bool InstallCommandListHooks(ID3D12Device* device)
    {
        ID3D12CommandAllocator*    alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
            REX::WARN("Starfield RT: CreateCommandAllocator failed");
            return false;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) {
            REX::WARN("Starfield RT: CreateCommandList failed");
            alloc->Release();
            return false;
        }
        auto** vtable = *reinterpret_cast<void***>(list);
        ID3D12GraphicsCommandList4* list4 = nullptr;
        const bool hasList4 = SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))) && list4 &&
                              *reinterpret_cast<void***>(list4) == vtable;
        if (list4) list4->Release();
        REX::INFO("Starfield RT: command list vtable {:p} in {} (List4 on same vtable: {})",
            static_cast<void*>(vtable), ModuleNameOf(vtable[0]), hasList4);

        struct Hook { std::size_t index; void* fn; bool needsList4; };
        const std::array<Hook, 20> hooks{ {
            { kSetComputeRootConstantBufferView, reinterpret_cast<void*>(&H_SetComputeRootCbv), false },   // 0.4.3
            { kSetComputeRootShaderResourceView, reinterpret_cast<void*>(&H_SetComputeRootSrv), false },
            { kSetComputeRootUnorderedAccessView, reinterpret_cast<void*>(&H_SetComputeRootUav), false },
            { kSetGraphicsRootSignature, reinterpret_cast<void*>(&H_SetGraphicsRootSignature), false },
            { kSetGraphicsRootDescriptorTable, reinterpret_cast<void*>(&H_SetGraphicsRootDescriptorTable), false },
            { kSetGraphicsRootConstantBufferView, reinterpret_cast<void*>(&H_SetGraphicsRootConstantBufferView), false },
            { kClearDepthStencilView, reinterpret_cast<void*>(&H_ClearDepthStencilView), false },
            { kExecuteIndirect, reinterpret_cast<void*>(&H_ExecuteIndirect), false },
            { kDrawInstanced, reinterpret_cast<void*>(&H_DrawInstanced), false },
            { kDrawIndexedInstanced, reinterpret_cast<void*>(&H_DrawIndexedInstanced), false },
            { kDispatch, reinterpret_cast<void*>(&H_Dispatch), false },
            { kSetPipelineState, reinterpret_cast<void*>(&H_SetPipelineState), false },
            { kSetComputeRootSignature, reinterpret_cast<void*>(&H_SetComputeRootSignature), false },
            { kSetComputeRootDescriptorTable, reinterpret_cast<void*>(&H_SetComputeRootDescriptorTable), false },
            { kSetComputeRoot32BitConstant, reinterpret_cast<void*>(&H_SetComputeRoot32BitConstant), false },
            { kSetComputeRoot32BitConstants, reinterpret_cast<void*>(&H_SetComputeRoot32BitConstants), false },
            { kRSSetViewports, reinterpret_cast<void*>(&H_RSSetViewports), false },
            { kResourceBarrier, reinterpret_cast<void*>(&H_ResourceBarrier), false },
            { kOMSetRenderTargets, reinterpret_cast<void*>(&H_OMSetRenderTargets), false },
            { kBeginRenderPass, reinterpret_cast<void*>(&H_BeginRenderPass), true },
        } };
        bool ok = true;
        for (const auto& h : hooks) {
            if (h.needsList4 && !hasList4) continue;
            g_origListFn[h.index] = vtable[h.index];  // store first so concurrent callers can forward
            ok &= PatchPointer(&vtable[h.index], h.fn, nullptr);
        }
        list->Close();
        list->Release();
        alloc->Release();
        REX::INFO("Starfield RT: command list hooks {}", ok ? "installed" : "partially failed");
        return ok;
    }

    // ---------------------------------------------------------------- device report

    ID3D12Device* GetEngineDevice()
    {
        auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
        return renderer ? reinterpret_cast<ID3D12Device*>(renderer->GetDevice()) : nullptr;
    }

    void ReportDevice(ID3D12Device* device)
    {
        ID3D12Device5* device5 = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || !device5) {
            REX::WARN("Starfield RT: ID3D12Device5 unavailable - DXR unsupported");
            return;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
        if (SUCCEEDED(device5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) {
            REX::INFO("Starfield RT: device {:p} ({}), raytracing tier {}.{}", static_cast<void*>(device),
                ModuleNameOf(*reinterpret_cast<void**>(device)), static_cast<int>(opt5.RaytracingTier) / 10,
                static_cast<int>(opt5.RaytracingTier) % 10);
        }
        device5->Release();
    }

    // ---------------------------------------------------------------- 0.1.8: contact-shadow mask test
    // TiledDeferredContactShadowsBlurY writes the final R8_UNORM contact shadow mask that
    // TiledDeferredLight reads (t4 space4). Its root table 4 is [SRV depth, SRV src, UAV dst, CBV],
    // so the destination UAV descriptor is table4 + 2. Right after BlurY's Execute we dispatch our
    // own checker pattern into that same descriptor, then restore the engine's compute state.

    constexpr UINT kMaskTableParam = 4;
    constexpr UINT kMaskUavOffset = 2;
    constexpr UINT kBlurThreadsPerGroup = 8;  // 0.1.18: BlurY runs 8x8 groups: 430x180 groups = 3440x1440 (output res, not the 1720x720 DLSS viewport)

    struct MaskTestPipeline
    {
        ID3D12RootSignature* rootSig = nullptr;
        ID3D12PipelineState* pso = nullptr;
        UINT                 descriptorSize = 0;
        bool                 ready = false;
    } g_maskTest;
    std::once_flag g_maskTestOnce;
    std::atomic<int> g_maskTestLogged{ 0 };

    bool CreateMaskTestPipeline(ID3D12Device* device)
    {
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = { 0, 0, 4 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = { 1, &range };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC desc{ 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* blob = nullptr;
        ID3DBlob* error = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
            REX::WARN("Starfield RT: mask test root signature serialize failed: {}",
                error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
            if (error) error->Release();
            return false;
        }
        const HRESULT hrSig = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_maskTest.rootSig));
        blob->Release();
        if (FAILED(hrSig)) {
            REX::WARN("Starfield RT: mask test CreateRootSignature failed ({:#x})", static_cast<unsigned>(hrSig));
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_maskTest.rootSig;
        pd.CS = { g_MaskTestPatternCS, sizeof(g_MaskTestPatternCS) };
        const HRESULT hrPso = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_maskTest.pso));
        if (FAILED(hrPso)) {
            REX::WARN("Starfield RT: mask test CreateComputePipelineState failed ({:#x})", static_cast<unsigned>(hrPso));
            return false;
        }
        g_maskTest.descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        REX::INFO("Starfield RT: mask test pipeline created (descriptor size {})", g_maskTest.descriptorSize);
        return true;
    }

    // Records the pattern into `list` right after BlurY. Returns a reason string for the log.
    const char* InjectMaskPattern(ID3D12GraphicsCommandList* list, bool blurDispatched)
    {
        if (g_maskTestMode == 0) return "off";
        ListState& s = t_state;
        if (!list || s.list != list) return "skipped: no tracked state for this list";
        if (!blurDispatched) return "skipped: BlurY recorded no dispatch";
        if (s.computeTables[kMaskTableParam].ptr == 0) return "skipped: BlurY bound no table 4";
        std::call_once(g_maskTestOnce, [] {
            if (auto* device = GetEngineDevice()) {
                g_maskTest.ready = CreateMaskTestPipeline(device);
            }
        });
        if (!g_maskTest.ready) return "skipped: pipeline unavailable";

        const D3D12_GPU_DESCRIPTOR_HANDLE uav{ s.computeTables[kMaskTableParam].ptr + kMaskUavOffset * g_maskTest.descriptorSize };
        const UINT extentX = s.lastDispatchX * kBlurThreadsPerGroup;
        const UINT extentY = s.lastDispatchY * kBlurThreadsPerGroup;
        const UINT consts[4]{ static_cast<UINT>(g_maskTestMode), static_cast<UINT>((std::max)(g_maskTestCell, 4)), extentX, extentY };
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // null resource = all UAV writes

        t_injecting = true;
        list->ResourceBarrier(1, &uavBarrier);  // wait for BlurY's writes
        list->SetComputeRootSignature(g_maskTest.rootSig);
        list->SetPipelineState(g_maskTest.pso);
        list->SetComputeRoot32BitConstants(0, 4, consts, 0);
        list->SetComputeRootDescriptorTable(1, uav);
        list->Dispatch((extentX + 7) / 8, (extentY + 7) / 8, 1);
        list->ResourceBarrier(1, &uavBarrier);  // make our writes visible to the next pass
        // Restore exactly what the engine had bound.
        if (s.computeRootSig) {
            list->SetComputeRootSignature(s.computeRootSig);
            for (UINT i = 0; i < kTrackedRootParams; ++i) {
                if (s.computeTables[i].ptr) list->SetComputeRootDescriptorTable(i, s.computeTables[i]);
                if (s.computeConstCount[i]) list->SetComputeRoot32BitConstants(i, s.computeConstCount[i], s.computeConsts[i].data(), 0);
            }
        }
        if (s.pso) list->SetPipelineState(s.pso);
        t_injecting = false;
        return "applied";
    }

    // ---------------------------------------------------------------- 0.1.9: scene buffer resolution
    // The global root table (compute/graphics root parameter 1, space 2) holds the scene records.
    // Offsets inside the table (from the PIX capture): t0 +2 instances (96 B), t2 +4 LOD entries (20 B),
    // t3 +5 mesh records (52 B), t5 +7 geometry pool (raw), t7 +9 transforms (48 B), t24 +26 meshlets (32 B).

    struct SceneTable { const char* name; std::uint32_t offset; std::uint32_t expectStride; };
    constexpr SceneTable kSceneTables[] = {
        { "instances", 2, 96 }, { "LOD entries", 4, 20 }, { "mesh records", 5, 52 },
        { "geometry pool", 7, 0 }, { "transforms", 9, 48 }, { "materials", 11, 288 }, { "veg materials", 13, 368 },
        { "meshlets", 26, 32 },
        { "alpha params", 28, 32 },
    };
    constexpr std::uint32_t kBindlessFirst = 32768;
    constexpr std::uint32_t kBindlessCount = 16384;
    constexpr std::uint32_t kPoolFirstSlot = 14;
    constexpr std::uint64_t kPoolWindow = 16ull * 1024 * 1024;

    const char* HeapTypeName(ID3D12Resource* r)
    {
        D3D12_HEAP_PROPERTIES props{};
        D3D12_HEAP_FLAGS flags{};
        if (FAILED(r->GetHeapProperties(&props, &flags))) return "reserved/unknown";
        switch (props.Type) {
        case D3D12_HEAP_TYPE_DEFAULT: return "DEFAULT";
        case D3D12_HEAP_TYPE_UPLOAD: return "UPLOAD";
        case D3D12_HEAP_TYPE_READBACK: return "READBACK";
        case D3D12_HEAP_TYPE_CUSTOM: return props.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE ? "CUSTOM(no CPU)" : "CUSTOM(CPU)";
        default: return "?";
        }
    }

    bool CpuReadable(ID3D12Resource* r)
    {
        D3D12_HEAP_PROPERTIES props{};
        D3D12_HEAP_FLAGS flags{};
        if (FAILED(r->GetHeapProperties(&props, &flags))) return false;
        return props.Type == D3D12_HEAP_TYPE_UPLOAD || props.Type == D3D12_HEAP_TYPE_READBACK ||
               (props.Type == D3D12_HEAP_TYPE_CUSTOM && props.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
    }

    void ReadMeshRecords(const rt::BufferView& v)
    {
        if (!CpuReadable(v.resource)) {
            REX::INFO("Starfield RT: mesh records are not CPU-readable ({}); a GPU readback will be needed", HeapTypeName(v.resource));
            return;
        }
        void* base = nullptr;
        const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(v.gpuva - v.resource->GetGPUVirtualAddress() + v.sizeBytes) };
        if (FAILED(v.resource->Map(0, &readRange, &base)) || !base) {
            REX::WARN("Starfield RT: Map of mesh records failed");
            return;
        }
        const auto* recs = static_cast<const std::uint8_t*>(base) + (v.gpuva - v.resource->GetGPUVirtualAddress());
        const std::size_t n = static_cast<std::size_t>(v.sizeBytes / 52);
        std::vector<std::uint8_t> copy(recs, recs + n * 52);  // one bulk read from write-combined memory
        const D3D12_RANGE noWrite{ 0, 0 };
        v.resource->Unmap(0, &noWrite);

        std::size_t used = 0, inPool = 0, skinned = 0;
        std::uint64_t verts = 0, indices = 0;
        std::uint32_t maxVerts = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto* r = copy.data() + i * 52;
            std::uint32_t handle, icount, vcount, bones;
            std::memcpy(&handle, r + 0, 4);
            std::memcpy(&bones, r + 28, 4);
            std::memcpy(&icount, r + 32, 4);
            std::memcpy(&vcount, r + 44, 4);
            if (!icount || !vcount) continue;
            ++used;
            const std::uint32_t slot = handle & 0x3FFF;
            inPool += slot >= kPoolFirstSlot && (slot - kPoolFirstSlot) * kPoolWindow < 4278190080ull;
            skinned += bones != 0;
            verts += vcount;
            indices += icount;
            maxVerts = (std::max)(maxVerts, vcount);
        }
        REX::INFO("Starfield RT: mesh records: {} slots, {} in use ({} with positions in the pool, {} skinned), "
                  "{} vertices / {} triangles total, largest mesh {} vertices",
            n, used, inPool, skinned, verts, indices / 3, maxVerts);
    }

    // 0.2.53: the global scene table (instances at +2, 96 B records). The RT build reads it from the contact-shadow blur
    // pass's compute root table 1, but at some locations that slot holds something else (an address in no tracked
    // heap), and nothing was built. Every valid sighting is remembered; a pass whose table 1 isn't valid uses it.
    std::atomic<std::uint64_t> g_sceneTable{ 0 };
    bool SceneTableValid(D3D12_GPU_DESCRIPTOR_HANDLE t)
    {
        if (!t.ptr || rt::HeapIndexOf(t) == UINT32_MAX) return false;
        rt::BufferView rec{};
        return rt::LookupGpuTable(t, 2, rec) && rec.stride == 96;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE SceneTableFor(D3D12_GPU_DESCRIPTOR_HANDLE bound, const char* where)
    {
        if (SceneTableValid(bound)) {
            g_sceneTable = bound.ptr;
            return bound;
        }
        // 0.2.54: newest recently bound table 1 that holds the scene table right now (same frame), else the last valid one.
        const std::uint32_t pos = g_table1Pos.load();
        for (std::uint32_t k = 1; k <= kTable1Ring && k <= pos; ++k) {
            const D3D12_GPU_DESCRIPTOR_HANDLE c{ g_table1Ring[(pos - k) % kTable1Ring].load(std::memory_order_relaxed) };
            if (c.ptr && c.ptr != bound.ptr && SceneTableValid(c)) {
                g_sceneTable = c.ptr;
                static std::atomic<std::uint64_t> used{ 0 };
                if (used.fetch_add(1) % 3600 == 0) {
                    REX::INFO("Starfield RT: {}: using the scene table recently bound by another pass (heap index {})", where, rt::HeapIndexOf(c));
                }
                return c;
            }
        }
        const D3D12_GPU_DESCRIPTOR_HANDLE cached{ g_sceneTable.load() };
        static std::atomic<std::uint64_t> fallbacks{ 0 };
        const auto n = fallbacks.fetch_add(1);
        if (n < 3 || n % 3600 == 0) {
            REX::INFO("Starfield RT: {}: root table 1 ({:#x}, heap index {}) is not the scene table; using the last valid one (heap index {}){}",
                where, bound.ptr, rt::HeapIndexOf(bound), cached.ptr ? rt::HeapIndexOf(cached) : UINT32_MAX, cached.ptr ? "" : ": none seen yet");
            if (n == 0) rt::LogHeaps(bound.ptr);
        }
        return cached;
    }

    void ResolveScene(D3D12_GPU_DESCRIPTOR_HANDLE table1)
    {
        rt::LogDeviceHookStats();
        if (!rt::DeviceHooksReady()) {
            REX::WARN("Starfield RT: scene resolution skipped: device hooks not installed");
            return;
        }
        const std::uint32_t tableIndex = rt::HeapIndexOf(table1);
        REX::INFO("Starfield RT: global scene table at heap index {}", tableIndex);
        rt::BufferView pool{}, meshes{};
        for (const auto& t : kSceneTables) {
            rt::BufferView v{};
            if (!rt::LookupGpuTable(table1, t.offset, v) || !v.resource) {
                REX::WARN("Starfield RT:   {:14s} (+{}): no buffer view tracked", t.name, t.offset);
                continue;
            }
            REX::INFO("Starfield RT:   {:14s} (+{}): resource {:p} gpuva {:#x} size {} stride {}{} heap {}{}", t.name, t.offset,
                static_cast<void*>(v.resource), v.gpuva, v.sizeBytes, v.stride, v.raw ? " raw" : "", HeapTypeName(v.resource),
                (t.expectStride && v.stride != t.expectStride) ? " (UNEXPECTED STRIDE)" : "");
            if (t.offset == 7) pool = v;
            if (t.offset == 5) meshes = v;
        }
        const std::uint32_t bindless = rt::CountBufferViews(table1, kBindlessFirst, kBindlessCount);
        REX::INFO("Starfield RT: bindless geometry slots: {} of {} have buffer views", bindless, kBindlessCount);
        rt::BufferView slot14{};
        if (rt::LookupHeapIndex(table1, kBindlessFirst + kPoolFirstSlot, slot14) && pool.resource) {
            REX::INFO("Starfield RT: slot 14 -> resource {:p} gpuva {:#x} size {} ({} the pool, pool gpuva {:#x})",
                static_cast<void*>(slot14.resource), slot14.gpuva, slot14.sizeBytes,
                slot14.resource == pool.resource ? "matches" : "does NOT match", pool.gpuva);
        }
        if (meshes.resource) {
            ReadMeshRecords(meshes);
        }
    }

    std::atomic<int> g_blurYCount{ 0 };
    std::atomic<std::uint64_t> g_frameCounter{ 0 };       // incremented at DeferredComposite
    std::uint64_t CurrentFrame() { return g_frameCounter.load(); }  // 0.5.36
    std::atomic<std::uint64_t> g_lastRtFrame{ ~0ull };   // frame of the last RT update
    // 0.2.25: the command list this frame's TLAS was built on (reflections only trace on that list).
    std::atomic<ID3D12GraphicsCommandList*> g_tlasList{ nullptr };
    std::atomic<std::uint64_t>              g_tlasListFrame{ ~0ull };
    // 0.5.8: flicker hunt. A one-frame dark flash in Neon (GI, lamps and reflections all missing at once) points at
    // frames where the GatherIndirect RT passes do not run. These count every such frame and its reason.
    std::atomic<std::uint64_t> g_rtRanFrame{ ~0ull };   // last frame the GI / lamps / reflections passes were recorded
    std::atomic<std::uint64_t> g_giExecFrame{ ~0ull };  // last frame GatherIndirect executed at all
    std::atomic<std::uint32_t> g_rtMissedFrames{ 0 }, g_rtStaleRuns{ 0 }, g_rtSkipLogs{ 0 };
    // 0.5.9: what this frame's RT build did. 0.5.8's log showed every dark frame as "TLAS built at frame N-1, last RT build
    // frame N": the build for this frame had started but never marked a TLAS. 1 = recording, 2 = recorded GPU work,
    // 3 = finished without recording anything (then nothing writes the BLAS/TLAS this frame and any list may trace).
    std::atomic<std::uint64_t>              g_buildFrame{ ~0ull };
    std::atomic<int>                        g_buildState{ 0 };
    std::atomic<ID3D12GraphicsCommandList*> g_buildList{ nullptr };
    std::atomic<std::uint32_t>              g_rtIdleRuns{ 0 }, g_buildEmptyLogs{ 0 };

    void RestoreComputeState(ID3D12GraphicsCommandList* list, const ListState& s)
    {
        if (s.computeRootSig) {
            list->SetComputeRootSignature(s.computeRootSig);
            for (UINT i = 0; i < kTrackedRootParams; ++i) {
                if (s.computeTables[i].ptr) list->SetComputeRootDescriptorTable(i, s.computeTables[i]);
                if (s.computeConstCount[i]) list->SetComputeRoot32BitConstants(i, s.computeConstCount[i], s.computeConsts[i].data(), 0);
                switch (s.computeRootKind[i]) {  // 0.4.3
                case 1: list->SetComputeRootConstantBufferView(i, s.computeRootVa[i]); break;
                case 2: list->SetComputeRootShaderResourceView(i, s.computeRootVa[i]); break;
                case 3: list->SetComputeRootUnorderedAccessView(i, s.computeRootVa[i]); break;
                default: break;
                }
            }
        }
        if (s.pso) list->SetPipelineState(s.pso);
    }
    constexpr int kResolveAfterBlurs = 900;  // a few seconds after the first lit frame

    // ---------------------------------------------------------------- generic pass hook

    std::once_flag      g_setupOnce;
    int                 g_frameTickPass = -1;  // index of DeferredComposite in g_passes
    int                 g_maskInjectPass = -1; // index of TiledDeferredContactShadowsBlurY
    int                 g_contactPass = -1;    // index of ScreenSpaceShadows (ContactShadows#0)
    thread_local D3D12_GPU_VIRTUAL_ADDRESS t_contactDataVa = 0;
    thread_local ID3D12GraphicsCommandList* t_contactList = nullptr;
    std::atomic<int>    g_sunLogged{ 0 };
    bool                g_listHooksReady = false;
    std::atomic<std::uint32_t> g_sequence{ 0 };  // global order of logged executes

    void LogPass(std::size_t index, const PassObservation& o, const ListState& s, std::uint32_t seq)
    {
        std::string line = std::format("Starfield RT: [{}] {} thread {}", seq, g_passes[index].name, GetCurrentThreadId());
        if (o.draws) {
            line += std::format(" | {} draw(s), first {} {}x{}", o.draws, o.indexed ? "indexed" : "non-indexed", o.drawCount, o.drawInstances);
        }
        if (o.dispatches) {
            line += std::format(" | {} dispatch(es), first {}x{}x{}", o.dispatches, o.dispX, o.dispY, o.dispZ);
        }
        if (!o.draws && !o.dispatches) {
            line += " | no draws/dispatches";
        }
        if (s.list && s.list == o.list) {
            line += std::format(" | RTs {} dsv {} viewport {}x{}", s.rtvCount, s.hasDsv, s.viewport.Width, s.viewport.Height);
        }
        REX::INFO("{}", line);
        for (std::uint32_t i = 0; i < o.barrierCount; ++i) {
            const auto& b = o.barriers[i];
            if (b.uav) {
                REX::INFO("Starfield RT:     UAV barrier  {:p} {}", static_cast<void*>(b.resource), DescribeResource(b.resource));
            } else {
                REX::INFO("Starfield RT:     transition   {:p} {} : {} -> {}", static_cast<void*>(b.resource),
                    DescribeResource(b.resource), StatesToString(b.before), StatesToString(b.after));
            }
        }
    }

    using ExecuteFn = std::uintptr_t (*)(void*, void*, void*);

    void ArmGlassSurvey(const char* why)
    {
        for (auto& c : g_glassSurveyCount) c = 0;
        const int id = g_glassSurveyId.fetch_add(1) + 1;
        g_glassSurveyUntil = g_frameCounter.load() + 3;
        REX::INFO("Starfield RT: [G1] glass survey #{} armed ({}) for frames {}..{}", id, why, g_frameCounter.load(), g_frameCounter.load() + 3);
    }

    template <std::size_t I>
    std::uintptr_t HookedExecute(void* self, void* ctx, void* resources)
    {
        auto& pass = g_passes[I];
        const auto original = reinterpret_cast<ExecuteFn>(pass.original);
        struct OitFlag  // 0.2.39 (G2): table-4 binds inside this pass get the glass probe swap
        {
            bool prev;
            explicit OitFlag(bool on) : prev(t_inOitTransparency) { if (on) t_inOitTransparency = true; }
            ~OitFlag() { t_inOitTransparency = prev; }
        } oitFlag(static_cast<int>(I) == g_oitTransparencyPass);
        if (static_cast<int>(I) == g_oitCompositePass && g_listHooksReady) {
            // 0.2.51 (G3): after the OIT composite (glass blended into the HDR target), add RT reflections on thin glass.
            const auto r = original(self, ctx, resources);
            const char* outcome = "no composite draw seen";
            auto* list = t_state.list;
            const auto t3 = t_state.graphicsTables[3];
            if (list && t_state.rtvCount >= 1 && t3.ptr) {
                ID3D12Resource* target = nullptr;
                DXGI_FORMAT fmt{};
                rt::BufferView dv{}, mv{};
                const std::uint32_t base = rt::HeapIndexOf(t3);
                if (!rt::GetTlasAddress() || rt::SceneSettling()) {  // 0.3.7: also the first seconds after a load
                    rt::ForgetReflectionInputs();  // 0.2.63: never reuse pre-load inputs after the load
                    // 0.2.59: like the main reflection pass, never trace while the scene is loading: BLASes are freed and
                    // rebuilt then, and the last TLAS can still point at them (0.2.58 hung the GPU on a save load here).
                    outcome = "skipped: scene loading";
                } else if (!rt::LookupRtv(t_state.rtvs[0].ptr, target, fmt) || !target) {
                    outcome = "skipped: HDR target not tracked";
                } else if (base == UINT32_MAX || !rt::LookupGpuTableAny(t3, 0, dv) || !dv.texture || !rt::LookupGpuTableAny(t3, 2, mv) ||
                           !mv.texture || mv.format != DXGI_FORMAT_R32_FLOAT) {
                    outcome = "skipped: depth / moment SRVs not where expected (composite table 3 +0 / +2)";
                } else {
                    t_injecting = true;
                    outcome = rt::RecordGlassReflections(GetEngineDevice(), list, target, base + 0, base + 2, rt::GetTlasAddress());
                    if (std::string_view(outcome) == "applied") RestoreComputeState(list, t_state);
                    // 0.3.3: keep this frame's final lit image (glass included) for next frame's GI.
                    const char* cap = rt::RecordGiCapture(GetEngineDevice(), list, target, base + 0, base + 2);
                    if (std::string_view(cap) == "applied") RestoreComputeState(list, t_state);
                    static const char* capLast = nullptr;
                    if (cap != capLast) {
                        capLast = cap;
                        REX::INFO("Starfield RT: [GI] last-frame image capture: {}", cap);
                    }
                    t_injecting = false;
                }
            }
            static std::atomic<int> logged{ 0 };
            static const char* last = nullptr;
            if (outcome != last || logged.fetch_add(1) % 3600 == 0) {
                if (outcome != last) logged = 1;
                last = outcome;
                REX::INFO("Starfield RT: [G3] thin-glass reflections: {}", outcome);
            }
            return r;
        }
        if constexpr (I >= kFirstGlassPass) {
            // 0.2.38 (G1): glass survey. Every execution is counted; inside an armed window the first 2 are dumped.
            const bool armed = g_listHooksReady && g_frameCounter.load() <= g_glassSurveyUntil.load();
            const int seen = g_glassSurveySeen[I].fetch_add(1);
            if (seen == 0) REX::INFO("Starfield RT: [G1] {} executes (first time, frame {})", pass.name, g_frameCounter.load());
            if (armed && g_glassSurveyCount[I].fetch_add(1) < 2) {
                REX::INFO("Starfield RT: [G1] {} Execute (survey #{}, frame {}, thread {}, self {:p})", pass.name, g_glassSurveyId.load(),
                    g_frameCounter.load(), GetCurrentThreadId(), self);
                const char* prevName = t_glassPassName;
                const int prevDraws = t_glassDraws;
                char tag[96];
                std::snprintf(tag, sizeof(tag), "[G1] %s", pass.name);
                t_glassPassName = tag;
                t_glassDraws = 6;
                const auto r = original(self, ctx, resources);
                REX::INFO("Starfield RT: [G1] {} done ({} draw(s) not dumped beyond 6)", pass.name, t_glassDraws <= 0 ? "maybe" : "no");
                t_glassPassName = prevName;
                t_glassDraws = prevDraws;
                return r;
            }
        }
        if (static_cast<int>(I) == g_gatherIndirectPass) {
            // 0.2.22: R1 survey. First in-game execution, then once per F8 press.
            const bool inGame = rt::GetTlasAddress() != 0;
            g_giExecFrame = g_frameCounter.load();  // 0.5.8
            const bool survey = g_listHooksReady && inGame &&
                                (!g_indirectSurveyDone.exchange(true) || g_indirectSurveyRequest.exchange(false));
            static std::atomic<std::uint64_t> runs{ 0 };
            if (survey) {
                REX::INFO("Starfield RT: [R1] GatherIndirect Execute #{} (self {:p})", runs.load(), self);
            }
            runs.fetch_add(1);
            t_inGatherIndirect = true;
            t_surveyDraws = survey ? 4 : -1;
            const auto r = original(self, ctx, resources);
            t_inGatherIndirect = false;
            t_surveyDraws = -1;
            // 0.2.25 (R2): debug views over the probe specular (RT1), on the list that built this frame's TLAS.
            if (g_listHooksReady && (rt::ReflectionDebugMode() != 0 || rt::GiMode() != 0 || rt::DirectLightsMode() != 0)) {  // 0.3.0 GI, 0.4.0 lamps
                const char* outcome = "no draw seen";
                auto* list = t_state.list;
                if (list && t_state.rtvCount >= 2) {
                    const std::uint64_t now = g_frameCounter.load(), tf = g_tlasListFrame.load();
                    const bool sameList = list == g_tlasList.load();
                    // 0.5.8: render threads record in parallel, so the frame counter (ticked at DeferredComposite) can move
                    // between the TLAS build and this pass. On the same command list the TLAS build is still earlier in
                    // GPU order, so one frame of lag is safe; before 0.5.8 that frame lost all RT lighting (the flicker).
                    const bool fresh = tf == now || (tf != ~0ull && tf + 1 == now);
                    const bool buildNow = g_buildFrame.load() == now;
                    const int buildState = buildNow ? g_buildState.load() : 0;
                    // 0.5.9: this frame's build finished without touching the BLAS/TLAS, so the last TLAS is complete and
                    // unchanged on the GPU: safe to trace from any command list.
                    const bool idleBuild = buildState == 3 && rt::GetTlasAddress() != 0;
                    if (!(sameList && fresh) && !idleBuild) {
                        outcome = "skipped: not the TLAS list this frame";
                        if (g_rtSkipLogs.fetch_add(1) < 40) {
                            const char* build = !buildNow ? "not started yet"
                                              : buildState == 1 ? "still being recorded"
                                              : buildState == 2 ? "recorded GPU work"
                                                                : "finished, nothing recorded";
                            REX::WARN("Starfield RT: [flicker] RT passes skipped, frame {}: {} (TLAS marked at frame {}; this frame's build: {}, on {} list; thread {})",
                                now, sameList ? "TLAS on this list is too old" : "TLAS was built on a different command list", tf, build,
                                g_buildList.load() == list ? "this" : "another", GetCurrentThreadId());
                        }
                    } else if (rt::SceneSettling()) {  // 0.3.7: also the first seconds after a load
                        outcome = "paused: scene loading";
                    } else {
                        rt::ReflectionInputs in{};
                        in.gbufferTable = t_state.graphicsTables[4];
                        rt::BufferView perScene{};
                        if (t_state.graphicsTables[3].ptr && rt::LookupGpuTable(t_state.graphicsTables[3], 0, perScene) && perScene.cbv &&
                            perScene.sizeBytes >= 20 * 16) {
                            in.perScene = perScene.gpuva;
                        }
                        in.tlas = rt::GetTlasAddress();
                        // 0.2.26 (R3): scene records from the global scene table (root table 1, same layout as compute).
                        if (const auto g = SceneTableFor(t_state.graphicsTables[1], "reflections"); g.ptr) {  // 0.2.53: validated
                            rt::BufferView rec{};
                            if (rt::LookupGpuTable(g, 2, rec) && rec.stride == 96) in.instances = rec.gpuva;
                            if (rt::LookupGpuTable(g, 4, rec) && rec.stride == 20) in.lods = rec.gpuva;
                            if (rt::LookupGpuTable(g, 5, rec) && rec.stride == 52) in.meshes = rec.gpuva;
                            if (rt::LookupGpuTable(g, 6, rec) && rec.stride == 32) in.tints = rec.gpuva;
                            if (rt::LookupGpuTable(g, 7, rec) && rec.raw && rec.resource) in.indexPool = rec.gpuva;
                            if (rt::LookupGpuTable(g, 11, rec) && rec.stride == 288) {
                                in.materials = rec.gpuva;
                                in.materialCount = static_cast<std::uint32_t>(rec.sizeBytes / 288);
                            }
                            if (rt::LookupGpuTable(g, 27, rec) && rec.stride == 40) in.textureSets = rec.gpuva;
                            if (rt::LookupGpuTable(g, 13, rec) && rec.stride == 368) {  // 0.2.49: alpha test
                                in.vegMaterials = rec.gpuva;
                                in.vegMaterialCount = static_cast<std::uint32_t>(rec.sizeBytes / 368);
                            }
                            if (rt::LookupGpuTable(g, 28, rec) && rec.stride == 32) {
                                in.alphaParams = rec.gpuva;
                                in.alphaParamsCount = static_cast<std::uint32_t>(rec.sizeBytes / 32);
                            }
                        }
                        in.contactData = (t_contactList == list) ? t_contactDataVa : 0;
                        DXGI_FORMAT fmt{};
                        rt::LookupRtv(t_state.rtvs[1].ptr, in.target, fmt);
                        t_injecting = true;
                        g_rtRanFrame = now;  // 0.5.8
                        if (!(sameList && fresh) && g_rtIdleRuns.fetch_add(1) < 20) {
                            REX::INFO("Starfield RT: [flicker] frame {}: ran on the unchanged TLAS (this frame's build recorded nothing)", now);
                        }
                        if (tf != now && g_rtStaleRuns.fetch_add(1) < 20) {
                            REX::INFO("Starfield RT: [flicker] frame {}: ran on the TLAS from frame {} (same command list) instead of skipping", now, tf);
                        }
                        if (rt::ReflectionDebugMode() != 0) {
                            outcome = rt::RecordReflectionDebug(GetEngineDevice(), list, in);
                            if (std::string_view(outcome) == "applied") RestoreComputeState(list, t_state);
                        } else {
                            outcome = "off";
                        }
                        // 0.3.0: RT diffuse GI into RT0 (the game's indirect diffuse), same inputs.
                        ID3D12Resource* rt0 = nullptr;
                        rt::LookupRtv(t_state.rtvs[0].ptr, rt0, fmt);
                        const char* gi = rt::RecordGi(GetEngineDevice(), list, in, rt0);
                        if (std::string_view(gi).starts_with("applied")) RestoreComputeState(list, t_state);
                        // 0.4.0: RT direct light from the game's lamps into RT0 (diffuse) and RT1 (specular).
                        const char* lamps = rt::RecordDirectLights(GetEngineDevice(), list, in, rt0, in.target);
                        // 0.5.0: guide images for DLSS Ray Reconstruction; 0.5.4: only while the game is actually running DLSS
                        if (rt::RayReconstructionWanted() && rt0 && rt::DlssRunning()) {
                            const D3D12_RESOURCE_DESC rd = rt0->GetDesc();
                            const char* rr = rt::RecordRrGuides(GetEngineDevice(), list, in, static_cast<UINT>(rd.Width), rd.Height);
                            if (std::string_view(rr) == "applied") RestoreComputeState(list, t_state);
                            static const char* rrLast = nullptr;
                            if (rr != rrLast) {
                                rrLast = rr;
                                REX::INFO("Starfield RT: [RR] guide images: {}", rr);
                            }
                        }
                        if (std::string_view(lamps) == "applied") RestoreComputeState(list, t_state);
                        static const char* lampsLast = nullptr;
                        if (lamps != lampsLast) {
                            lampsLast = lamps;
                            REX::INFO("Starfield RT: [lamps] RT lamps: {}", lamps);
                        }
                        t_injecting = false;
                        static std::atomic<int> giLogged{ 0 };
                        static const char* giLast = nullptr;
                        if (gi != giLast || giLogged.fetch_add(1) % 3600 == 0) {
                            if (gi != giLast) giLogged = 1;
                            giLast = gi;
                            REX::INFO("Starfield RT: [GI] {}: {}", rt::GiModeName(rt::GiMode()), gi);
                        }
                    }
                }
                static std::atomic<int> logged{ 0 };
                static std::atomic<int> lastMode{ 0 };
                const int mode = rt::ReflectionDebugMode();
                if (lastMode.exchange(mode) != mode) logged = 0;
                if (logged.fetch_add(1) < 3) {
                    REX::INFO("Starfield RT: reflection debug ({}): {}", rt::ReflectionDebugName(mode), outcome);
                }
            }
            return r;
        }
        // 0.2.6: while RT sun shadows are on, the game's sun cascades (ShadowMapInstance) draw nothing, so the
        // cascade maps stay empty (fully lit) and the RT shadow alone decides what is in sunlight.
        const bool replaceSun = I == kSunShadowMapPass && g_replaceGameSunShadows != 0 && rt::CurrentSunMode() != 0 && rt::GetTlasAddress() != 0;
        if (replaceSun && g_replaceGameSunShadows == 3) {
            // 0.2.10 default: let the pass draw normally (dropping its draws coincided with Frame Generation hangs),
            // then clear the depth targets it bound with the value the game clears them with, so the sun cascade
            // maps end the pass empty (fully lit).
            t_collectDsvs = true;
            t_passDsvs.clear();
            const auto r = original(self, ctx, resources);
            t_collectDsvs = false;
            int cleared = 0, unknown = 0;
            float value = 0.0f;
            if (t_state.list) {
                for (const auto& d : t_passDsvs) {
                    bool known = false;
                    {
                        std::lock_guard lock(g_dsvClearLock);
                        if (auto it = g_dsvClearValue.find(d.ptr); it != g_dsvClearValue.end()) { value = it->second; known = true; }
                    }
                    if (!known) { ++unknown; continue; }
                    Orig<void(STDMETHODCALLTYPE*)(L*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*)>(
                        kClearDepthStencilView)(t_state.list, d, D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
                    ++cleared;
                }
            }
            static std::atomic<std::uint64_t> logged{ 0 };
            if (logged.fetch_add(1) % 3600 == 0) {
                REX::INFO("Starfield RT: sun cascades: {} DSV(s) re-cleared to {} after ShadowMapInstance, {} with no known clear value "
                          "(those keep the game's shadows)", cleared, value, unknown);
            }
            return r;
        }
        if ((replaceSun && g_replaceGameSunShadows == 2) ||
            (g_skipPass.load(std::memory_order_relaxed) == static_cast<int>(I) && rt::CurrentSunMode() != 0)) {
            // 0.2.5 skip test: run the pass but drop its draw calls (state and return value stay the engine's).
            t_suppressDraws = true;
            const auto r = original(self, ctx, resources);
            t_suppressDraws = false;
            return r;
        }
        std::call_once(g_setupOnce, [] {
            if (auto* device = GetEngineDevice()) {
                ReportDevice(device);
                g_listHooksReady = InstallCommandListHooks(device);
            }
            // Never touch windows from a render thread: run on a throwaway thread so nothing
            // here can block the frame (no-op unless the PIX capturer is loaded).
            if (GetModuleHandleW(L"WinPixGpuCapturer.dll")) {
                std::thread([] { SetPixTargetWindow(); }).detach();
            }
        });
        if (static_cast<int>(I) == g_frameTickPass) {
            PixFrameTick();  // DeferredComposite runs once per frame
            {
                // 0.5.8: a frame that ends with no GI / lamps / reflections pass is a dark flash on screen.
                const std::uint64_t f = g_frameCounter.load();
                const bool wanted = g_listHooksReady && rt::GetTlasAddress() != 0 && !rt::SceneSettling() &&
                                    (rt::ReflectionDebugMode() != 0 || rt::GiMode() != 0 || rt::DirectLightsMode() != 0);
                if (wanted && g_rtRanFrame.load() != f) {
                    const std::uint32_t n = g_rtMissedFrames.fetch_add(1) + 1;
                    if (n <= 40) {
                        REX::WARN("Starfield RT: [flicker] frame {} ended without RT GI/lamps/reflections (GatherIndirect {}, last RT pass frame {}, TLAS frame {}, thread {})",
                            f, g_giExecFrame.load() == f ? "ran" : "did not run this frame", g_rtRanFrame.load(), g_tlasListFrame.load(), GetCurrentThreadId());
                    }
                }
                if (f % 600 == 599) {
                    REX::INFO("Starfield RT: [flicker] totals at frame {}: {} frame(s) without RT lighting, {} saved by the one-frame TLAS lag",
                        f, g_rtMissedFrames.load(), g_rtStaleRuns.load());
                }
            }
            g_frameCounter.fetch_add(1);
            rt::timer::FrameTick(g_frameCounter.load(), rt::CurrentSunMode(), rt::SoftShadowsOn(), rt::AlphaTestOn());
            if (g_frameCounter.load() % 600 == 300) {
                rt::LogVram(GetEngineDevice(), "every 600 frames");  // 0.2.33
                REX::INFO("Starfield RT: buffers held through descriptor slots: {}", rt::HeldBufferCount());  // 0.5.14
            }
            PollGameOptions(g_frameCounter.load());  // 0.2.36: in-game quality levels
            {
                static std::uint64_t firstInGame = 0;  // 0.2.38: automatic G1 glass survey ~10 s into the first scene
                if (!firstInGame && rt::GetTlasAddress() != 0) firstInGame = g_frameCounter.load();
                static bool autoDone = false;
                if (firstInGame && !autoDone && g_frameCounter.load() >= firstInGame + 600) {
                    autoDone = true;
                    ArmGlassSurvey("first scene");
                }
            }
            // 0.1.33: a watchdog thread checks for a dead GPU device every 200 ms. The render thread stops
            // reaching this point once the GPU faults, so an in-frame check alone never gets to log the DRED report.
            static std::once_flag watchdogOnce;
            std::call_once(watchdogOnce, [] {
                std::thread([] {
                    for (;;) {
                        Sleep(200);
                        if (rt::CheckDeviceRemoved(GetEngineDevice())) break;
                    }
                }).detach();
                REX::INFO("Starfield RT: GPU crash watchdog started");
            });
            static bool toggleWasDown = false;  // live RT sun shadow on/off for same-view comparisons
            const bool toggleDown = g_sunToggleKey && (GetAsyncKeyState(g_sunToggleKey) & 0x8000) != 0;
            if (toggleDown && !toggleWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {  // 0.5.59: Ctrl+Numpad 3 cycles RT shadows -> hit-distance -> alignment views
                    const int cur = rt::CurrentSunMode();
                    const int next = cur == 4 ? 5 : cur == 5 ? 2 : 4;
                    rt::SetSunShadowsLive(next);
                    static constexpr const char* kName[6] = { "off", "on", "on", "A/B", "debug: hit distance (black = blocked at the surface, grey = far, normal = unblocked)",
                        "debug: alignment (dark = our scene doesn't match the game's depth)" };
                    Announce("RT sun shadows", kName[next]);
                } else {
                    const int m = rt::ToggleSunShadows(); Announce("RT sun shadows", m == 0 ? "off (game shadows)" : std::format("on (mode {})", m));
                }
            }
            toggleWasDown = toggleDown;
            static bool probeWasDown = false;  // debug probe: what do rays through the screen centre hit?
            const bool probeDown = g_probeKey && (GetAsyncKeyState(g_probeKey) & 0x8000) != 0;
            if (probeDown && !probeWasDown && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {  // 0.5.28: Ctrl+Numpad . = baseline
                g_baselineOff = true;
                ApplyBaselineOff("Ctrl+Numpad .");
                Announce("Baseline", "every RT effect off, game look (turn effects on one at a time with their keys)");
            } else if (probeDown && !probeWasDown) {
                rt::RequestProbe();
                rt::RequestSkinDump();           // 0.5.52
                rt::RequestGlassDump();         // 0.2.62: what the thin-glass pass hits at the screen centre
                g_indirectSurveyRequest = true;  // 0.2.22: also re-run the R1 GatherIndirect survey
                ArmGlassSurvey("F8");            // 0.2.38: and the G1 glass survey
                Announce("Probe", "captured, see the log");
            }
            probeWasDown = probeDown;
            static bool reflWasDown = false;  // 0.2.25: R2 reflection debug views
            const bool reflDown = g_reflectDebugKey && (GetAsyncKeyState(g_reflectDebugKey) & 0x8000) != 0;
            if (reflDown && !reflWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {  // 0.5.13: Ctrl+Numpad 0 = Ray Reconstruction on/off
                    Announce("DLSS Ray Reconstruction", rt::ToggleRrPaused() ? "off (plain DLSS)" : "on");
                } else {
                    const int mode = rt::CycleReflectionDebug();
                    if (mode == 5) {  // 0.5.43: RT reflections on -> Ray Reconstruction on too (it settles their noise)
                        rt::SetRrPaused(false);
                        Announce("Reflections", std::format("{} + DLSS Ray Reconstruction on", rt::ReflectionDebugName(mode)));
                    } else {
                        Announce("Reflections", rt::ReflectionDebugName(mode));
                    }
                }
            }
            reflWasDown = reflDown;
            static bool upWasDown = false, downWasDown = false;  // 0.2.28: live reflection brightness
            const bool upDown = g_reflBrighterKey && (GetAsyncKeyState(g_reflBrighterKey) & 0x8000) != 0;
            const bool dnDown = g_reflDarkerKey && (GetAsyncKeyState(g_reflDarkerKey) & 0x8000) != 0;
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (upDown && !upWasDown) {
                if (ctrl)  // 0.5.24: Ctrl+Numpad + / - = glossier / softer reflections
                    Announce("Reflection gloss", std::format("sharper, fReflectionGloss {:.2f}", rt::AdjustReflectionGloss(-0.05f)));
                else
                    Announce("Reflection brightness", std::format("up, fReflectionProbeAmbient {:.2f}", rt::AdjustReflectionBrightness(1.25f)));
            }
            if (dnDown && !downWasDown) {
                if (ctrl)
                    Announce("Reflection gloss", std::format("softer, fReflectionGloss {:.2f}", rt::AdjustReflectionGloss(0.05f)));
                else
                    Announce("Reflection brightness", std::format("down, fReflectionProbeAmbient {:.2f}", rt::AdjustReflectionBrightness(0.8f)));
            }
            upWasDown = upDown;
            downWasDown = dnDown;
            static bool alphaWasDown = false;  // 0.1.23: alpha-tested (leaf) shadows on/off
            const bool alphaDown = g_alphaToggleKey && (GetAsyncKeyState(g_alphaToggleKey) & 0x8000) != 0;
            if (alphaDown && !alphaWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000)  // 0.5.24: Ctrl+Numpad * = stronger reflections
                    Announce("Reflection strength", std::format("stronger, fReflectionStrength {:.2f}", rt::AdjustReflectionStrength(0.1f)));
                else
                    Announce("Leaf/fence cutouts in RT", rt::ToggleAlphaTest() ? "on" : "off (solid cards)");
            }
            alphaWasDown = alphaDown;
            static bool softWasDown = false;  // 0.1.30: soft shadows on/off
            const bool softDown = g_softToggleKey && (GetAsyncKeyState(g_softToggleKey) & 0x8000) != 0;
            if (softDown && !softWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000)  // 0.5.24: Ctrl+Numpad / = weaker reflections
                    Announce("Reflection strength", std::format("weaker, fReflectionStrength {:.2f}", rt::AdjustReflectionStrength(-0.1f)));
                else
                    Announce("Soft RT shadows", rt::ToggleSoftShadows() ? "on" : "off (hard)");
            }
            softWasDown = softDown;
            static bool glassWasDown = false;  // 0.2.39 (G2): the game's probe reflection on glass on/off
            const bool glassDown = g_glassReflKey && (GetAsyncKeyState(g_glassReflKey) & 0x8000) != 0;
            if (glassDown && !glassWasDown) {
                const bool hide = !g_hideGlassProbe.load();
                g_hideGlassProbe = hide;
                Announce("Game cubemap reflections on glass", hide ? "hidden" : "shown");
            }
            glassWasDown = glassDown;
            static bool sharpWasDown = false;  // 0.2.42: sharp view through glass on/off
            const bool sharpDown = g_sharpGlassKey && (GetAsyncKeyState(g_sharpGlassKey) & 0x8000) != 0;
            if (sharpDown && !sharpWasDown) {
                const bool sharp = !g_sharpGlass.load();
                g_sharpGlass = sharp;
                Announce("View through glass", sharp ? "sharp" : "game blur");
            }
            sharpWasDown = sharpDown;
            static bool thickWasDown = false;  // 0.2.48: thick-glass see-through layer on/off
            const bool thickDown = g_thickGlassKey && (GetAsyncKeyState(g_thickGlassKey) & 0x8000) != 0;
            if (thickDown && !thickWasDown) {
                const bool skip = !g_skipThickGlassLayer.load();
                g_skipThickGlassLayer = skip;
                Announce("Thick glass extra layer", skip ? "hidden" : "shown (game)");
            }
            thickWasDown = thickDown;
            static bool lampWasDown = false;  // 0.4.0: RT lamps on / off
            const bool lampDown = g_rtLampKey && (GetAsyncKeyState(g_rtLampKey) & 0x8000) != 0;
            if (lampDown && !lampWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {  // 0.5.23: Ctrl+Numpad 2 = RR colour-before-transparency guide
                    Announce("Ray Reconstruction smoke/glass guide", rt::ToggleColorBeforeTransparency() ? "on" : "off");
                } else {
                    Announce("Lamps", rt::ToggleDirectLights() ? "ray traced (game lamps off)" : "game's own lamps");
                }
            }
            lampWasDown = lampDown;
            static bool giWasDown = false;  // 0.3.0: RT GI on -> debug -> off
            const bool giDown = g_giKey && (GetAsyncKeyState(g_giKey) & 0x8000) != 0;
            if (giDown && !giWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {  // 0.5.7: Ctrl+Numpad 1 = RR raw mode
                    const bool on = rt::ToggleRrRaw();
                    Announce("Ray Reconstruction input", on ? (rt::RayReconstructionRunning() ? "raw (RR denoises)" : "raw (waiting for RR: DLSS on?)")
                                                            : "our filters + RR");
                } else {
                    Announce("Global illumination", rt::GiModeName(rt::CycleGi()));
                }
            }
            giWasDown = giDown;
            static bool lightWasDown = false;  // 0.2.58: game lights in reflections on/off
            const bool lightDown = g_localLightKey && (GetAsyncKeyState(g_localLightKey) & 0x8000) != 0;
            if (lightDown && !lightWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000)  // 0.5.32: Ctrl+Numpad 5 = probe floor for off-screen reflection hits
                    Announce("Off-screen reflection hits", rt::ToggleOffscreenProbeFloor() ? "never darker than the game's probe"
                                                                                             : "ray-traced only (can go dark)");
                else
                    Announce("Game lamps in reflections/GI", rt::ToggleLocalLights() ? "on" : "off (sun + ambient only)");
            }
            lightWasDown = lightDown;
            static bool thinWasDown = false;  // 0.2.51 (G3): thin-glass RT reflections on/off
            const bool thinDown = g_thinGlassKey && (GetAsyncKeyState(g_thinGlassKey) & 0x8000) != 0;
            if (thinDown && !thinWasDown) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {  // 0.5.13: Ctrl+Numpad 4 = untagged-window search on/off
                    Announce("Thin glass: untagged-window search", rt::ToggleThinGlassFallback() ? "on" : "off (tagged glass only)");
                } else {
                    static const char* const kThin[3] = { "off", "on", "debug colours (yellow/white = reflected, magenta = glass missed, blue = no glass)" };
                    Announce("RT reflections on thin glass", kThin[rt::ToggleThinGlass()]);
                }
            }
            thinWasDown = thinDown;
            static bool sepWasDown = false;  // 0.2.44: glass skipped by shadow and reflection rays, on/off
            const bool sepDown = g_glassSeparateKey && (GetAsyncKeyState(g_glassSeparateKey) & 0x8000) != 0;
            if (sepDown && !sepWasDown) {
                Announce("See-through surfaces in reflections", rt::ToggleGlassSeparate() ? "skipped" : "solid");
            }
            sepWasDown = sepDown;
            static bool litWasDown = false;  // 0.2.4: lit test
            const bool litDown = g_litTestKey && (GetAsyncKeyState(g_litTestKey) & 0x8000) != 0;
            if (litDown && !litWasDown) {
                const bool litOn = rt::ToggleLitTest();
                REX::INFO("Starfield RT: lit test -> {} (mode {})", litOn ? "ON: RT forced fully lit, game sun shadow maps off" : "off",
                    rt::CurrentSunMode());
            }
            litWasDown = litDown;
            static bool skipWasDown = false;  // 0.2.5: cycle the shadow-map pass whose draws are dropped
            const bool skipDown = g_skipCycleKey && (GetAsyncKeyState(g_skipCycleKey) & 0x8000) != 0;
            if (skipDown && !skipWasDown) {
                const int cur = g_skipPass.load();
                int next = cur < 0 ? static_cast<int>(kFirstShadowCandidate) : cur + 1;
                if (next >= static_cast<int>(kShadowCandidateEnd)) next = -1;
                g_skipPass = next;
                REX::INFO("Starfield RT: skip test -> {}", next < 0 ? "none" : g_passes[next].name);
            }
            skipWasDown = skipDown;
        }
        if (g_listHooksReady && static_cast<int>(I) == g_contactPass) {
            // ContactShadows#0: its root table 4 starts with the contact-shadow Data CBV (light direction).
            const auto result = original(self, ctx, resources);
            rt::BufferView data{};
            if (t_state.computeTables[kMaskTableParam].ptr &&
                rt::LookupGpuTable(t_state.computeTables[kMaskTableParam], 0, data) && data.cbv && data.sizeBytes >= 96) {
                t_contactDataVa = data.gpuva;
                t_contactList = t_state.list;
            }
            return result;
        }
        if (g_listHooksReady && static_cast<int>(I) == g_maskInjectPass) {
            const std::uint32_t before = t_dispatchCount;
            const auto result = original(self, ctx, resources);
            const char* outcome = InjectMaskPattern(t_state.list, t_dispatchCount != before);
            const D3D12_GPU_DESCRIPTOR_HANDLE sceneTable = SceneTableFor(t_state.computeTables[1], "RT build");  // 0.2.53
            if (g_blurYCount.fetch_add(1) == kResolveAfterBlurs && sceneTable.ptr) {
                ResolveScene(sceneTable);
            }
            // 0.1.10: once per frame, build BLASes/TLAS on this command list.
            // 0.1.13: then trace the sun shadow into the contact mask on the same list.
            const std::uint64_t frame = g_frameCounter.load();
            if (t_state.list && sceneTable.ptr && g_lastRtFrame.exchange(frame) != frame) {
                t_injecting = true;
                LARGE_INTEGER cpuStart, cpuEnd, qpf;
                QueryPerformanceCounter(&cpuStart);
                rt::timer::BeginFrame(GetEngineDevice(), t_state.list, frame);  // 0.2.1 profiler
                rt::BufferView sceneCb{};  // 0.2.3: camera position for TLAS distance culling
                D3D12_GPU_VIRTUAL_ADDRESS sceneVa =
                    (rt::LookupGpuTable(t_state.computeTables[3], 0, sceneCb) && sceneCb.cbv) ? sceneCb.gpuva : 0;
                // 0.2.66: in some interiors (Neon) this pass binds no PerSceneConstants. Without the camera position no
                // skinned character (NPCs, the player) was ever added to the RT scene there. The scene table holds a copy.
                if (!sceneVa && rt::LookupGpuTable(sceneTable, 34, sceneCb) && sceneCb.cbv && sceneCb.sizeBytes >= 53008) {
                    sceneVa = sceneCb.gpuva;
                }
                g_buildState = 1;  // 0.5.9
                g_buildList = t_state.list;
                g_buildFrame = frame;
                const bool recorded = rt::BlasFrameUpdate(GetEngineDevice(), t_state.list, sceneTable, sceneVa, frame);
                g_buildState = recorded ? 2 : 3;
                if (!recorded && rt::GetTlasAddress() && g_buildEmptyLogs.fetch_add(1) < 20) {
                    REX::INFO("Starfield RT: [flicker] frame {}: RT build recorded no BLAS/TLAS work", frame);
                }
                // 0.5.9: a build that recorded nothing leaves last frame's TLAS complete and unchanged, so it is still usable
                // here (sun shadows) and for the lighting passes. Before, about 1 frame in 250 lost every RT effect this way.
                const bool usable = rt::GetTlasAddress() != 0 && (recorded || !rt::SceneSettling());
                if (usable) {
                    g_tlasList = t_state.list;
                    g_tlasListFrame = frame;
                }
                const char* sun = "no TLAS this frame";
                if (usable && rt::SceneLoading()) {
                    sun = "paused: scene loading";  // 0.2.12: no RT shadow rays while the scene fills in after a load
                } else if (usable) {
                    rt::SunShadowInputs in{};
                    in.blurTable = t_state.computeTables[kMaskTableParam];
                    rt::BufferView perScene{};
                    if (rt::LookupGpuTable(t_state.computeTables[3], 0, perScene) && perScene.cbv) {
                        in.perScene = perScene.gpuva;
                    }
                    if (rt::LookupGpuTable(sceneTable, 34, perScene) && perScene.cbv && perScene.sizeBytes >= 53008) {
                        in.probePerScene = perScene.gpuva;  // 0.2.61: lets F8 work where the shadow pass has no PerScene
                    }
                    in.contactData = (t_contactList == t_state.list) ? t_contactDataVa : 0;
                    in.tlas = rt::GetTlasAddress();
                    in.width = t_state.lastDispatchX * kBlurThreadsPerGroup;
                    in.height = t_state.lastDispatchY * kBlurThreadsPerGroup;
                    rt::BufferView rec{};
                    if (rt::LookupGpuTable(sceneTable, 2, rec) && rec.stride == 96) in.instances = rec.gpuva;
                    if (rt::LookupGpuTable(sceneTable, 4, rec) && rec.stride == 20) in.lods = rec.gpuva;
                    if (rt::LookupGpuTable(sceneTable, 5, rec) && rec.stride == 52) in.meshes = rec.gpuva;
                    if (rt::LookupGpuTable(sceneTable, 11, rec) && rec.stride == 288) {
                        in.materials = rec.gpuva;
                        in.materialCount = static_cast<std::uint32_t>(rec.sizeBytes / 288);
                    }
                    if (rt::LookupGpuTable(sceneTable, 7, rec) && rec.raw && rec.resource) {
                        in.indexPool = rec.gpuva;
                    }
                    if (rt::LookupGpuTable(sceneTable, 28, rec) && rec.stride == 32) {
                        in.alphaParams = rec.gpuva;
                        in.alphaParamsCount = static_cast<std::uint32_t>(rec.sizeBytes / 32);
                    }
                    if (rt::LookupGpuTable(sceneTable, 13, rec) && rec.stride == 368) {
                        in.vegMaterials = rec.gpuva;
                        in.vegMaterialCount = static_cast<std::uint32_t>(rec.sizeBytes / 368);
                    }
                    in.frame = frame;
                    sun = rt::RecordSunShadows(GetEngineDevice(), t_state.list, in);
                }
                if (recorded) {
                    RestoreComputeState(t_state.list, t_state);
                }
                QueryPerformanceCounter(&cpuEnd);
                QueryPerformanceFrequency(&qpf);
                rt::timer::EndFrame(t_state.list,
                    static_cast<double>(cpuEnd.QuadPart - cpuStart.QuadPart) * 1000.0 / static_cast<double>(qpf.QuadPart));
                t_injecting = false;
                if (g_sunLogged.fetch_add(1) < 4 || frame % 1800 == 0) {
                    REX::INFO("Starfield RT: sun shadows (frame {}): {}", frame, sun);
                }
            }
            if (g_maskTestLogged.fetch_add(1) < 6) {
                REX::INFO("Starfield RT: mask test after BlurY (list {:p}, dispatch {}x{}, table4 {:#x}): {}",
                    static_cast<void*>(t_state.list), t_state.lastDispatchX, t_state.lastDispatchY,
                    t_state.computeTables[kMaskTableParam].ptr, outcome);
            }
            return result;
        }
        if (!g_listHooksReady || pass.logged.load(std::memory_order_relaxed) >= kLogsPerPass) {
            return original(self, ctx, resources);
        }
        PassObservation obs;
        obs.pass = static_cast<int>(I);
        PassObservation* parent = t_obs;  // passes can nest (subgraphs)
        t_obs = &obs;
        const auto result = original(self, ctx, resources);
        t_obs = parent;
        if (pass.logged.fetch_add(1) < kLogsPerPass) {
            LogPass(I, obs, t_state, g_sequence.fetch_add(1));
        }
        return result;
    }

    template <std::size_t... I>
    constexpr std::array<void*, sizeof...(I)> MakeHookTable(std::index_sequence<I...>)
    {
        return { reinterpret_cast<void*>(&HookedExecute<I>)... };
    }
    const auto g_hookFns = MakeHookTable(std::make_index_sequence<kPassCount>{});

    void InstallPassHooks()
    {
        const auto base = ModuleBase();
        int installed = 0;
        for (std::size_t i = 0; i < kPassCount; ++i) {
            auto& p = g_passes[i];
            auto** slot = reinterpret_cast<void**>(base + p.vtableRva + kExecuteSlot * sizeof(void*));
            if (*slot != reinterpret_cast<void*>(base + p.executeRva)) {
                REX::WARN("Starfield RT: {} Execute slot mismatch; skipped", p.name);
                continue;
            }
            if (PatchPointer(slot, g_hookFns[i], &p.original)) {
                ++installed;
                if (std::string_view(p.name) == "DeferredComposite") {
                    g_frameTickPass = static_cast<int>(i);
                } else if (std::string_view(p.name) == "TiledDeferredContactShadowsBlurY") {
                    g_maskInjectPass = static_cast<int>(i);
                } else if (std::string_view(p.name) == "ScreenSpaceShadows") {
                    g_contactPass = static_cast<int>(i);
                } else if (std::string_view(p.name) == "GatherIndirect") {
                    g_gatherIndirectPass = static_cast<int>(i);
                } else if (std::string_view(p.name) == "MomentBasedOITRenderTransparency") {
                    g_oitTransparencyPass = static_cast<int>(i);
                } else if (std::string_view(p.name) == "CompositeMomentBasedOIT") {
                    g_oitCompositePass = static_cast<int>(i);
                }
            }
        }
        REX::INFO("Starfield RT: pass survey hooks installed on {}/{} passes", installed, kPassCount);
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
    if (!sfse || sfse->RuntimeVersion() != kSupportedRuntime) {
        return false;
    }
    SFSE::Init(sfse);
    REX::INFO("Starfield RT: Phase 0 pass survey loading");
    LoadSettingsAndTools();  // must run before the game creates its D3D12 device
    rt::InstallDeviceCreationHook();  // likewise: catches every descriptor the engine writes from startup
    // 0.4.11: Streamline hooks; 0.5.0: + DLSS Ray Reconstruction ([RT] bDLSSRayReconstruction)
    rt::InstallStreamlineLogging(GetPrivateProfileIntW(L"RT", L"bDLSSRayReconstruction", 0,
        (PluginDirectory() / L"StarfieldRT.ini").wstring().c_str()) != 0);
    rt::SetRrRaw(GetPrivateProfileIntW(L"RT", L"bRRRawMode", 0, (PluginDirectory() / L"StarfieldRT.ini").wstring().c_str()) != 0);  // 0.5.7
    rt::SetColorBeforeTransparency(GetPrivateProfileIntW(L"RT", L"bRRColorBeforeTransparency", 0,
        (PluginDirectory() / L"StarfieldRT.ini").wstring().c_str()) != 0);  // 0.5.23
    InstallPassHooks();
    // 0.5.20: look up the Gameplay Options globals on the main thread once the game's data is loaded.
    if (const auto* messaging = SFSE::GetMessagingInterface()) {
        messaging->RegisterListener([](SFSE::MessagingInterface::Message* msg) {
            if (msg && msg->type == SFSE::MessagingInterface::kPostDataLoad) LookupGameOptionsMainThread();
        });
    }
    return true;
}
