-- Only the plugin needs CommonLibSSE-NG (and MSVC); on Linux the game-free targets below are built with gcc and
-- the plugin is skipped. The test is on the host, not the platform: the platform is not known yet when xmake reads
-- this file for its command line, and the library's options (--skyrim_vr, --skse_patch_safety) must be known by then.
if is_host("windows") then
    -- Skyrim VR is not supported (docs/DESIGN.md), and without it the library needs neither the OpenVR headers nor
    -- rapidcsv. The library's "patch safety" option downloads MinHook; nothing here uses it. Both are also given on
    -- the `xmake f` line in CONTRIBUTING.md and the workflows, since a configuration made before this file was read
    -- keeps the library's own defaults.
    set_config("skyrim_vr", false)
    set_config("skse_patch_safety", false)
    includes("lib/commonlibsse-ng")
end

-- set project constants
set_project("SkyrimCoop")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
if not is_plat("windows") then
    -- gcc's and clang's -Wextra warn on every aggregate init that leaves fields to their defaults, which the
    -- code does on purpose everywhere (MSVC, the plugin's compiler, says nothing)
    add_cxxflags("-Wno-missing-field-initializers")
end

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("enet v1.3.18")  -- pinned: a new enet release must not slip into a release build unnoticed

-- keep the build machine's paths (and so the user name) out of the binaries: __FILE__ loses the
-- project directory, and the .pdb is referred to by name only
if is_plat("windows") then
    add_cxxflags("/d1trimfile:" .. os.projectdir():gsub("/", "\\") .. "\\", { force = true })
    add_ldflags("/PDBALTPATH:%_PDB%", { force = true })
    add_shflags("/PDBALTPATH:%_PDB%", { force = true })
end

-- static analysis: xmake f --analyze=y runs the MSVC analyser on our targets
option("analyze")
    set_default(false)
    set_showmenu(true)
    set_description("Run the MSVC code analyser (/analyze) on our own sources")
option_end()

-- only our own sources are analysed and reported; <...> headers (lib/, enet, the SDK) are external
local function analyze()
    if has_config("analyze") and is_plat("windows") then
        add_cxxflags("/analyze", "/analyze:external-", "/external:anglebrackets", "/external:W0", { force = true })
    end
end

-- protocol, networking helpers and the relay server, shared by every target
target("SkyrimCoopCommon")
    set_kind("static")
    add_files("common/**.cpp")
    add_headerfiles("common/**.h")
    add_includedirs("common", { public = true })
    add_includedirs("src")  -- for headers with no game dependency
    analyze()
    add_packages("enet", { public = true })
    if is_plat("windows") then
        add_syslinks("ws2_32", "winmm", { public = true })
    end

-- client plugin, loaded into the game by SKSE (Windows and MSVC only: it needs CommonLibSSE-NG and the game)
if is_plat("windows") then
target("SkyrimCoop")
    -- tools/package.ps1 sets it to the label and commit ("rc3 1a2b3c4"), shown in the log
    add_defines('SKYRIMCOOP_BUILD_LABEL="' .. (os.getenv("SKYRIMCOOP_LABEL") or "") .. '"')
    -- Some of the library's headers bring in Windows.h. Without this it brings the old winsock.h with it, and ENet's
    -- winsock2.h then fails to compile (every socket type defined twice).
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
    add_rules("commonlibsse-ng.plugin", {
        name = "SkyrimCoop",
        author = "Jolley71717",
        description = "Co-op multiplayer for Skyrim Special Edition",
        options = { address_library = true }
    })

    add_deps("SkyrimCoopCommon")
    analyze()

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_syslinks("ws2_32", "bcrypt", "shell32", "ole32", "user32")
end

-- standalone dedicated server
target("SkyrimCoopServer")
    set_kind("binary")
    add_deps("SkyrimCoopCommon")
    analyze()
    add_files("server/**.cpp")

-- fake player for testing without a second copy of the game
target("SkyrimCoopBot")
    set_kind("binary")
    add_deps("SkyrimCoopCommon")
    analyze()
    add_files("bot/**.cpp")

-- unit tests for the protocol, the server and the game-free client logic (no game needed). Built by a
-- plain `xmake build`, so whatever runs the tests after a build never runs an old binary.
target("SkyrimCoopTests")
    set_kind("binary")
    add_deps("SkyrimCoopCommon")
    analyze()
    add_files("tests/**.cpp")
    -- Unoptimised, and with the standard headers every test file would re-read precompiled: test bodies run once,
    -- and optimising them is most of a clean build's time. SkyrimCoopCommon (the server and the protocol) is still
    -- built as it ships; the header-only client logic the tests include is compiled here, unoptimised.
    set_optimize("none")
    set_pcxxheader("tests/pch.h")
    add_includedirs("src")  -- for headers with no game dependency, e.g. Settings.h
    add_headerfiles("tests/**.h")
