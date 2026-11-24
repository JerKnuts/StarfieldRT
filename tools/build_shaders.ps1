# Compiles every shader in shaders/ into the headers in src/generated/ (0.5.12). Run from the project root.
# Usage: powershell -File tools\build_shaders.ps1
$dxc = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
$jobs = @(
    @("cs_6_0", "main",         "g_MaskTestPatternCS",   "MaskTestPatternCS",   "MaskTestPattern"),
    @("cs_6_0", "main",         "g_CopyGeometryCS",      "CopyGeometryCS",      "CopyGeometry"),
    @("cs_6_6", "main",         "g_BuildInstancesCS",    "BuildInstancesCS",    "BuildInstances"),
    @("cs_6_6", "main",         "g_SkinVerticesCS",      "SkinVerticesCS",      "SkinVertices"),
    @("cs_6_6", "main",         "g_RtProbeCS",           "RtProbeCS",           "RtProbe"),
    @("cs_6_6", "main",         "g_RtSunShadowCS",       "RtSunShadowCS",       "RtSunShadow"),
    @("cs_6_6", "Denoise",      "g_RtSunDenoiseCS",      "RtSunDenoiseCS",      "RtSunShadow"),
    @("cs_6_6", "main",         "g_RtReflectDebugCS",    "RtReflectDebugCS",    "RtReflectDebug"),
    @("cs_6_6", "Resolve",      "g_RtReflectResolveCS",  "RtReflectResolveCS",  "RtReflectDebug"),
    @("cs_6_6", "Glass",        "g_RtGlassCS",           "RtGlassCS",           "RtReflectDebug"),
    @("cs_6_6", "GiTrace",      "g_RtGiTraceCS",         "RtGiTraceCS",         "RtReflectDebug"),
    @("cs_6_6", "GiTemporal",   "g_RtGiTemporalCS",      "RtGiTemporalCS",      "RtReflectDebug"),
    @("cs_6_6", "GiResolve",    "g_RtGiResolveCS",       "RtGiResolveCS",       "RtReflectDebug"),
    @("cs_6_6", "GiCapture",    "g_RtGiCaptureCS",       "RtGiCaptureCS",       "RtReflectDebug"),
    @("cs_6_6", "ReflTemporal", "g_RtReflTemporalCS",    "RtReflTemporalCS",    "RtReflectDebug"),
    @("cs_6_6", "DlCull",       "g_RtDlCullCS",          "RtDlCullCS",          "RtReflectDebug"),
    @("cs_6_6", "DlShade",      "g_RtDlShadeCS",         "RtDlShadeCS",         "RtReflectDebug"),
    @("cs_6_6", "LightCopy",    "g_RtLightCopyCS",       "RtLightCopyCS",       "RtReflectDebug"),
    @("cs_6_6", "RrGuides",     "g_RtRrGuidesCS",        "RtRrGuidesCS",        "RtReflectDebug")
)
$failed = 0
foreach ($j in $jobs) {
    & $dxc -T $j[0] -E $j[1] -O3 -Qstrip_debug -Qstrip_reflect -Vn $j[2] -Fh "src\generated\$($j[3]).h" "shaders\$($j[4]).hlsl"
    if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $($j[3])"; $failed++ }
}
Write-Host "shaders: $($jobs.Count - $failed) ok, $failed failed"
exit $failed
