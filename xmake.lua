add_rules("mode.debug", "mode.release", "mode.releasedbg")

add_repositories("liteldev-repo https://github.com/LiteLDev/xmake-repo.git")
add_requires("levilamina 0.13.x", { alias = "levilamina", configs = { shared = true } })
add_requires("levibuildscript")

-- All LL plugins MUST use MD (release DLL CRT) to match BDS.
-- Using MT or MDd here causes heap corruption across DLL boundaries.
if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("endweave")
    add_rules("@levibuildscript/modpacker")
    set_kind("shared")
    set_languages("cxx20")
    set_plat("windows")
    set_arch("x64")

    -- Two source files only.
    add_files("src/codec.cpp", "src/plugin.cpp")
    add_includedirs("src")
    add_packages("levilamina")

    add_defines(
        "NOMINMAX",          -- prevent windows.h min/max macros clobbering std::min/max
        "WIN32_LEAN_AND_MEAN",
        "UNICODE", "_UNICODE",
        "_AMD64_",           -- required by some BDS headers for intrinsic selection
        "ENTT_PACKED_PAGE=128"  -- EnTT config must match BDS build
    )

    add_cxxflags(
        "/EHsc",             -- synchronous exceptions, required by BDS
        "/Zc:preprocessor",  -- standard-compliant preprocessor (__VA_OPT__ etc.)
        "/Zc:__cplusplus",   -- __cplusplus == 202002L, not 199711L
        "/Zc:inline",        -- remove unreferenced inline functions
        "/utf-8",            -- source + execution charset UTF-8 (LL string literals)
        -- Suppress unavoidable BDS fake-header noise:
        "/wd4099",  -- type seen as struct now seen as class
        "/wd4251",  -- needs dll-interface
        "/wd4275",  -- non-DLL-interface base
        "/wd4201",  -- nameless struct/union
        "/wd4324",  -- structure padded due to __declspec(align)
        { force = true }
    )

    add_ldflags("/SUBSYSTEM:WINDOWS", { force = true })

    if is_mode("debug") then
        add_defines("EW_DEBUG")
        set_optimize("none")
        set_symbols("debug")
        add_cxxflags("/Zi", "/Od", "/RTC1", { force = true })
        add_ldflags("/DEBUG:FULL", { force = true })

    elseif is_mode("releasedbg") then
        -- Release performance + PDB for crash dump analysis on live servers
        set_optimize("faster")
        set_symbols("debug")
        add_cxxflags("/O2", "/Zi", { force = true })
        add_ldflags("/DEBUG:FULL", { force = true })

    else -- release (default, CI)
        set_optimize("aggressive")
        set_symbols("hidden")
        add_cxxflags("/O2", "/GL", "/Gy", { force = true })
        add_ldflags("/LTCG", "/OPT:REF", "/OPT:ICF", { force = true })
    end

    after_build(function(target)
        local dll = target:targetfile()
        if os.isfile(dll) then
            print(string.format("[endweave] built → %s  (%d KiB)",
                dll, math.floor(os.filesize(dll) / 1024)))
        end
    end)
