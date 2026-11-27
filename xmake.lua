set_xmakever("3.0.0")
set_project("StarfieldRT")
set_version("0.5.136")
set_arch("x64")
set_languages("c++23")
add_rules("mode.release")
includes("external/commonlibsf")

target("StarfieldRT")
    add_rules("commonlibsf.plugin", {
        name = "Starfield RT",
        author = "JerKnuts",
        description = "Ray-traced lighting, shadows and reflections (Phase 0 diagnostics)"
    })
    add_files("src/*.cpp")
    add_includedirs("external/streamline/include")  -- 0.4.11: NVIDIA Streamline SDK 2.14.16 headers (no linking)
    add_syslinks("d3d12", "dxgi", "dxguid")
