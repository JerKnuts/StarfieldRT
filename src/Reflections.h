#pragma once
// RT reflections (PLAN.md Phase 2). R2: debug views written over the game's probe specular
// (Indirect#0 SV_Target1), right after GatherIndirect's full-screen draw.

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace rt
{
    struct ReflectionSettings
    {
        float debugScale = 1.0f;      // brightness of the debug views
        float maxDistance = 3000.0f;  // mirror ray length, world units
        float sunIntensity = 1.0f;    // 0.2.26 R3: placeholder hit lighting (R4 calibrates against the game's units)
        float ambient = 0.15f;
        // 0.2.27 (R4)
        bool  startOn = true;         // [RT] iReflections=1: RT reflections on at start (F2 cycles)
        float fadeStart = 0.45f;      // roughness where RT starts fading back to the game's probe reflection (0.2.29: glossy rays)
        float fadeEnd = 0.7f;         // roughness past which only the probe reflection is left
        float blurPx = 10.0f;         // 0.2.29: glossy filter radius in pixels at roughness 1 (x sqrt(roughness))
        float glossRoughScale = 0.65f; // 0.5.24: reflections only (replace-all): roughness x this (lower = sharper, glossier)
        float glossStrength = 0.5f;    // 0.5.24: reflections only (replace-all): reflection weight x this
        float probeAmbient = 2.5f;    // hit lighting: share of the game's probe radiance used as ambient (0.2.28: was 1.0)
        float minWeight = 0.045f;     // 0.2.28: no ray where the reflection weight is below this
        bool  replaceAll = true;      // 0.2.31: RT at every roughness; the game's cubemap reflection is never shown
        float replaceMinWeight = 0.0f;  // 0.2.31: in replace-all mode, fainter pixels fade out (0.2.34 smooth 0.01; 0.2.37: 0 = trace every pixel)
        int   traceScale = 2;         // 0.2.34: 1 = a ray per pixel, 2 = a ray per 2x2 block
        bool  hitShadows = true;      // 0.2.34: sun shadow ray at each reflection hit
        unsigned smoothGlassTypes = 4;  // 0.2.46: bit n = G-buffer material type n is glass (0.2.47: default type 2 = glass)
        float glassRoughScale = 0.0f;   // 0.2.46: 0 = mirror-smooth glass, 1 = the game's roughness
        int   thinGlass = 1;            // 0.2.51 (G3): RT reflections on thin (see-through) glass: 0 off, 1 on, 2 stage debug view
        float thinGlassStrength = 3.0f; // 0.2.51: scales them (1 = Fresnel of real glass)
        bool  thinGlassFallback = true;  // 0.4.10: untagged windows found by "any object in front of the wall"
        float thinGlassMinCoverage = 0.001f;  // 0.2.51: OIT optical depth b0 below this = no see-through surface; < 0 = no gate
        bool  localLights = true;       // 0.2.58: light reflection hits with the game's local lights
        bool  offscreenProbeFloor = true;  // 0.5.32: off-screen reflection hits never darker than the game's probe
        float localLightScale = 1.0f;   // 0.2.58: their brightness
        int   localShadowRays = 4;      // 0.2.58: shadow rays per hit (first lights that contribute)
        // 0.3.0: RT diffuse GI (replaces the game's indirect diffuse, GatherIndirect SV_Target0)
        int   giMode = 1;               // 0 off, 1 on, 2 debug: GI light alone on grey surfaces
        float giStrength = 1.0f;        // output scale (1 = physical); 0.5.46: was 2 (light surfaces came out white)
        float giBounce = 0.5f;          // share of the game's ambient lighting ray hits (multi-bounce stand-in)
        float giRayLength = 100.0f;     // world units
        float giHistory = 48.0f;        // frames accumulated (more = smoother, slower to react); 0.3.4: 32, 0.4.6: 48
        float giRadius = 6.0f;          // spatial filter radius in trace cells (0.4.6: was 4)
        int   giTraceScale = 2;         // 1 = a ray per pixel, 2 = one per 2x2 pixels
        int   giRays = 1;               // 0.4.8: rays per trace cell (Ultra: 2)
        float giScreenLight = 1.0f;     // 0.3.3: light taken from last frame's image at GI hits (0 = off, 1 = physical)
        bool  reflTemporal = true;      // 0.3.4: temporal accumulation of RT reflections
        float emissiveScale = 1.0f;     // 0.3.6: material emission at ray hits (0 = off)
        int   emissiveModel = 0;        // 0.3.6: 0 adaptive (2^offset / exposure), 1 luminous emittance (absolute)
        bool  rtLamps = true;           // 0.4.0: the game's lamps are drawn by us with RT shadows (the game's are switched off)
        float rtLampScale = 1.0f;       // 0.4.0: their brightness (1 = the game's)
        int   hitLampShadowRays = 0;    // 0.4.5: lamp shadow rays at reflection / GI hits (0 = unshadowed: much less noise)
    };
    void SetReflectionSettings(const ReflectionSettings& s);

    // Hotkey: cycles off -> RT reflections (5) -> debug views: world normal (1), mirror hit (2), roughness (3),
    // RT reflection colour (4) -> off. Returns the new mode.
    int  CycleReflectionDebug();
    int  ReflectionDebugMode();
    // 0.2.28: live brightness of reflected objects (PageUp / PageDown): multiplies the probe light by `factor`.
    float AdjustReflectionBrightness(float factor);
    // 0.5.28: baseline (everything off) setters.
    void SetReflectionModeRaw(int mode);  // 0 = off (the game's reflections), 5 = RT reflections
    void SetGiModeRaw(int mode);          // 0 off, 1 on, 2 debug
    void SetDirectLightsRaw(int mode);    // 0 = the game's lamps, 1 = RT lamps
    void SetThinGlassMode(int mode);      // 0 off, 1 on, 2 debug colours
    void SetReflectionLook(float gloss, float strength);
    // 0.5.24: live art controls for RT reflections. Each returns the new value (delta 0 = just read it).
    float AdjustReflectionGloss(float delta);     // roughness scale, 0.2..1 (lower = sharper)
    float AdjustReflectionStrength(float delta);  // weight scale, 0.5..3
    const char* ReflectionDebugName(int mode);

    struct ReflectionInputs
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferTable{};  // GatherIndirect root table 4 (space4: t1 normal, t2 material, t6 depth)
        D3D12_GPU_VIRTUAL_ADDRESS   perScene = 0;     // PerSceneConstants CBV
        D3D12_GPU_VIRTUAL_ADDRESS   tlas = 0;
        ID3D12Resource*             target = nullptr; // Indirect#0 SV_Target1 (probe specular), in RENDER_TARGET state
        // 0.2.26 (R3): scene records from the global scene table (GatherIndirect root table 1) and the sun direction.
        D3D12_GPU_VIRTUAL_ADDRESS   instances = 0, lods = 0, meshes = 0, tints = 0, indexPool = 0, materials = 0, textureSets = 0;
        std::uint32_t               materialCount = 0;
        D3D12_GPU_VIRTUAL_ADDRESS   vegMaterials = 0, alphaParams = 0;  // 0.2.49: alpha test for reflection rays
        std::uint32_t               vegMaterialCount = 0, alphaParamsCount = 0;
        D3D12_GPU_VIRTUAL_ADDRESS   contactData = 0;  // contact-shadow Data CBV (row 5 = sun direction, camera space)
    };

    // Records the debug view on `list` (caller restores compute state and the PSO). Returns a reason string.
    const char* RecordReflectionDebug(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in);

    // 0.2.51 (G3): RT reflections on thin glass, right after CompositeMomentBasedOIT. `target` is the HDR scene target
    // (R11G11B10, RENDER_TARGET state); depthIndex / momentIndex are heap indices of the composite's depth and b0 SRVs.
    // Uses the inputs of this frame's reflection pass. Caller restores compute state and the PSO when it returns "applied".
    const char* RecordGlassReflections(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* target,
        std::uint32_t depthIndex, std::uint32_t momentIndex, D3D12_GPU_VIRTUAL_ADDRESS tlas);
    int ToggleThinGlass();  // 0.2.55: cycles on -> debug -> off -> on

    // 0.3.0: RT diffuse GI into the game's indirect diffuse target (GatherIndirect RT0, R11G11B10, RENDER_TARGET state),
    // with the same inputs as the reflection pass. Caller restores compute state when it returns "applied".
    const char* RecordGi(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in, ID3D12Resource* target);
    int GiMode();
    int CycleGi();  // on -> debug (GI light alone) -> off -> on
    const char* GiModeName(int mode);
    // 0.4.0: RT direct lighting from the game's local lamps, added into GatherIndirect RT0 (diffuse) / RT1 (specular).
    // While it runs, the game's own tiled lighting skips its lamps (see CaptureLocalLights).
    const char* RecordDirectLights(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in,
        ID3D12Resource* rt0, ID3D12Resource* rt1);
    int DirectLightsMode();
    // 0.5.7: RR raw mode (reflections and GI unfiltered while DLSS Ray Reconstruction runs). Toggle returns the new state.
    void SetRrRaw(bool on);
    // 0.5.13: Ctrl+Numpad 4: the untagged-window fallback of the thin-glass pass on/off. Returns the new state.
    bool ToggleThinGlassFallback();
    bool ToggleRrRaw();
    // 0.5.0: DLSS Ray Reconstruction guide images at the DLSS input size, written on `list` (caller restores compute state
    // when it returns "applied"). GetRrGuides: diffuse albedo, specular albedo, normal+roughness, specular hit distance,
    // in PIXEL|NON_PIXEL_SHADER_RESOURCE state.
    const char* RecordRrGuides(ID3D12Device* device, ID3D12GraphicsCommandList* list, const ReflectionInputs& in, UINT width, UINT height);
    bool GetRrGuides(ID3D12Resource* out[4], std::uint32_t& width, std::uint32_t& height);
    // 0.5.23: the game's lit image from just before transparencies, for DLSS-RR (kBufferTypeColorBeforeTransparency).
    void NoteColorBeforeTransparency(ID3D12Resource* tex);
    Microsoft::WRL::ComPtr<ID3D12Resource> GetColorBeforeTransparency();
    void SetColorBeforeTransparency(bool on);
    bool ToggleColorBeforeTransparency();  // Ctrl+Numpad 2; returns the new state
    int ToggleDirectLights();
    // 0.3.3: after the OIT composite: keep this frame's lit image (HDR target, RENDER_TARGET state) and per-pixel distance
    // (depth SRV heap index) for next frame's GI. Caller restores compute state when it returns "applied".
    const char* RecordGiCapture(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* target, std::uint32_t depthIndex,
        std::uint32_t momentIndex);  // 0.3.9: OIT optical depth: covered pixels are not used

    // 0.2.58: the game's local-light buffer (TiledDeferredLight cb2 space4, 64 KB) and its index ranges (t0 space4, 48 B),
    // captured when the main view's lighting pass binds them. 0.2.60: copied into our own buffer on that pass's command
    // list (`list`), so reflections read a stable copy. Returns false if the source buffers aren't known upload buffers.
    // 0.4.3: records a GPU copy of them on `list` (changes its compute state: the caller restores it) and returns the
    // slot buffer; switchOff = point the game's ranges descriptor at the slot's emptied copy (byte 65760) now.
    ID3D12Resource* CaptureLocalLights(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS lightCb,
        D3D12_GPU_VIRTUAL_ADDRESS counts, D3D12_GPU_DESCRIPTOR_HANDLE table, bool& switchOff);
    bool ToggleLocalLights();
    bool ToggleOffscreenProbeFloor();  // 0.5.32: Ctrl+Numpad 5
    // 0.2.62: F8: the next thin-glass pass logs what it hits at the screen centre (instance + material record).
    void RequestGlassDump();
    // 0.2.63: drop the remembered reflection inputs and the light copy (call while the scene loads: the buffers they
    // point at are freed and reallocated, and the glass pass must not reuse them on the first frame afterwards).
    void ForgetReflectionInputs();
}
