#pragma once
// 0.4.11: Streamline (sl.interposer.dll) hooks on Starfield.exe's import table. Step 1 toward DLSS Ray Reconstruction:
// log only, every call is passed through unchanged.

namespace rt
{
    // Call once at plugin load (before the game calls slInit, so the loaded features are logged too).
    // 0.5.0: rayReconstruction = load DLSS-RR and run it in place of DLSS-SR once its inputs are ready.
    void InstallStreamlineLogging(bool rayReconstruction);
    bool RayReconstructionWanted();
    // 0.5.13: pause Ray Reconstruction live (the game's DLSS-SR runs instead). Returns true when now paused.
    bool ToggleRrPaused();
    void SetRrPaused(bool paused);  // 0.5.28: baseline off = paused
    // 0.5.4: the game ran DLSS within the last 2 seconds (the RR guide pass is skipped otherwise).
    bool DlssRunning();
    // 0.5.7: DLSS Ray Reconstruction ran in place of DLSS-SR on the latest DLSS frame.
    bool RayReconstructionRunning();
}
