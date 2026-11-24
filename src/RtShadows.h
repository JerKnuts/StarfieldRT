#pragma once
// Ray-traced sun shadows written into Starfield's contact shadow mask.

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>

namespace rt
{
    struct SunShadowSettings
    {
        int   mode = 0;              // 0 off, 1 combine (min with contact shadows), 2 replace (debug), 3 left-half A/B
        float maxDistance = 2000.0f; // ray length, world units
        float biasBase = 0.05f;
        float biasPerUnit = 0.002f;
        // 0.1.30: soft shadows (cone-traced rays + penumbra-aware filter)
        bool  soft = true;
        int   rays = 4;              // shadow rays per pixel
        float sunRadiusDeg = 0.15f;   // apparent angular radius of the sun
        int   maxBlurPx = 4;        // largest filter radius
        int   softScale = 2;         // 0.2.2: soft shadows trace one sample per softScale x softScale pixels
    };

    void SetSunShadowSettings(const SunShadowSettings& s);

    // Live on/off toggle (hotkey): switches between mode 0 and the configured mode. Returns the new mode.
    int ToggleSunShadows();
    void SetSunShadowsLive(int mode);  // 0.5.28: 0 = the game's shadows (baseline off)

    // The RT sun shadow mode in effect right now (ini mode, or the live toggle). 0 = off.
    int CurrentSunMode();

    // 0.1.30: live soft-shadow on/off (hotkey). Returns the new state.
    bool ToggleSoftShadows();
    // 0.2.4: lit test on/off (forces RT mode 6: mask = 1 everywhere, game sun shadow maps still off).
    bool ToggleLitTest();
    bool SoftShadowsOn();

    struct SunShadowInputs
    {
        D3D12_GPU_DESCRIPTOR_HANDLE blurTable{};    // BlurY root table 4: [+0 SRV depth, +2 UAV mask]
        D3D12_GPU_VIRTUAL_ADDRESS   perScene = 0;   // PerSceneConstants CBV
        D3D12_GPU_VIRTUAL_ADDRESS   probePerScene = 0;  // 0.2.61: PerSceneConstants from the scene table (+34), F8 probe only
        D3D12_GPU_VIRTUAL_ADDRESS   contactData = 0;// contact-shadow Data CBV (light direction in row 5)
        D3D12_GPU_VIRTUAL_ADDRESS   tlas = 0;
        std::uint32_t               width = 0, height = 0;
        // For the debug probe: scene record buffers (global table +2, +4, +5) and the frame number.
        D3D12_GPU_VIRTUAL_ADDRESS   instances = 0, lods = 0, meshes = 0;
        // 0.1.23: material entries (global table +11, 288 B) for the alpha test.
        D3D12_GPU_VIRTUAL_ADDRESS   materials = 0, vegMaterials = 0;      // +11 (288 B) and +13 (368 B)
        std::uint32_t               materialCount = 0, vegMaterialCount = 0;
        D3D12_GPU_VIRTUAL_ADDRESS   alphaParams = 0;                      // 0.1.26: +28 (32 B)
        D3D12_GPU_VIRTUAL_ADDRESS   indexPool = 0;                        // 0.1.29: geometry pool +7 (uint16 indices)
        std::uint32_t               alphaParamsCount = 0;
        std::uint64_t               frame = 0;
    };

    // Debug probe (hotkey): next frame, trace one primary ray through the screen centre and one sun ray
    // from that surface, and log what each hit (instance/LOD/mesh records).
    void RequestProbe();

    // Records the RT shadow dispatch into `list` (caller restores compute state). Returns a reason string.
    const char* RecordSunShadows(ID3D12Device* device, ID3D12GraphicsCommandList* list, const SunShadowInputs& in);
}
