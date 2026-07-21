add_rules("mode.debug", "mode.release")

set_warnings("more")

add_defines("WIN32", "_WIN32", "UNICODE", "_UNICODE")
set_encodings("source:utf-8")
-- set_languages("c++23")
-- NOTE: `/std:c++23preview` is an MSVC-only spelling that clang-cl rejects
-- (warning: argument unused -> falls back to C++14, breaking consteval /
-- std::optional). `/std:c++latest` is accepted by BOTH cl and clang-cl and
-- enables the same (and newer) features, so it is the portable choice.
add_cxxflags("/std:c++latest", {force = true})
set_fpmodels("precise") -- Default
if is_mode("release") then
    set_exceptions("none")
    set_optimize("smallest")
    set_runtimes("MT")
    -- vc-ltl5 has no prebuilt package for arm64, so skip it there (arm64
    -- falls back to the MSVC static runtime). Applying it unconditionally
    -- makes `xmake f -a arm64` fail to resolve libcmt.lib etc.
    if not is_arch("arm64") then
        add_requires("vc-ltl5")
    end
    add_defines("NDEBUG")
    add_cxflags("/Gy", "/GR-", {force = true})
    add_ldflags("/DYNAMICBASE", "/OPT:REF", "/OPT:ICF", {force = true})
    set_policy("build.optimization.lto", true)
end

if is_mode("debug") then
    set_runtimes("MTd")
    add_defines("_DEBUG")
    add_ldflags("/DYNAMICBASE")
end

target("detours")
    set_kind("static")
    add_includedirs("detours/src", {public=true})
    add_files(
        "detours/src/detours.cpp",
        "detours/src/disasm.cpp",
        "detours/src/image.cpp",
        "detours/src/modules.cpp"
    )
    if is_arch("x86") then
        add_defines("_X86_")
        add_files("detours/src/disolx86.cpp")
    elseif is_arch("x64") then
        add_defines("_AMD64_")
        add_files("detours/src/disolx64.cpp")
    elseif is_arch("arm64") then
        add_defines("_ARM64_")
        add_files("detours/src/disolarm64.cpp")
    end
    add_cxflags("-Wno-unknown-pragmas", "-Wno-reorder-ctor", "-Wno-unused-local-typedef", {tools = "clang_cl", force = true})

target("mini_gzip")
    set_kind("static")
    add_includedirs("mini_gzip", {public = true})
    add_files("mini_gzip/miniz.c", "mini_gzip/mini_gzip.c")
    add_cxflags("/wd5287", "/wd4267", {tools = "cl"}) 

-- Standalone GUI update window (replaces swap.bat). No CRT: tiny (~20-40 KB),
-- embedded as RCDATA 1001 into version.dll. This target's after_build writes
-- src/updater_res.rc pointing at this freshly linked exe (file-path RCDATA).
target("chrome_green_updater")
    set_kind("binary")
    set_targetdir("$(builddir)/$(mode)")
    set_basename("chrome_green_updater")
    add_files("src/updater_gui.c")
    add_defines("WIN32", "_WIN32", "UNICODE", "_UNICODE", "_WIN32_WINNT=0x0601")
    set_encodings("source:utf-8")
    -- no C runtime: custom entry, only system libs, tiny binary
    add_cflags("/GS-", "/Gs16777216", {force = true})
    add_ldflags("/NODEFAULTLIB", "/ENTRY:wWinMain", "/SUBSYSTEM:WINDOWS", {force = true})
    add_syslinks("kernel32", "user32", "gdi32", "advapi32", "gdiplus", "shcore")
    -- Embed this freshly-linked exe into version.dll as RCDATA 1001 via
    -- src/updater_res.rc. This runs in the exe's after_build, so the exe file
    -- is guaranteed to exist at that moment.
    --
    -- NOTE: xmake deletes build/release/chrome_green_updater.exe after it
    -- builds the dll (the dll embeds the exe via this rc but does NOT link it,
    -- so xmake treats the dependency's output as disposable). That is fine:
    -- scripts/build.bat builds THIS target FIRST (step 1, forced with -r) so
    -- the exe is on disk when the dll's rc is compiled in step 2, and the dll
    -- embedding is captured before xmake cleans the exe. Never run a bare
    -- `xmake build` without step 1 or it will hit RC2135.
    after_build(function(target)
        local exepath = target:targetfile()
        if not (exepath:match("^%a:[\\/]") or exepath:match("^\\")) then
            exepath = os.projectdir() .. "/" .. exepath
        end
        exepath = exepath:gsub("/", "\\")
        exepath = exepath:gsub("^\\([a-zA-Z])\\", function(d)
            return d:upper() .. ":\\"
        end)
        -- RC string literals treat backslashes as escapes, so double them.
        local rcpath = exepath:gsub("\\", "\\\\")
        local rc = '1001 RCDATA "' .. rcpath .. '"\r\n'
        local rf = io.open("src/updater_res.rc", "w")
        rf:write(rc)
        rf:close()
    end)

target("chrome_green")
    set_kind("shared")
    set_targetdir("$(builddir)/$(mode)")
    set_basename("version")
    add_deps("detours")
    add_deps("mini_gzip")
    add_deps("chrome_green_updater")
    add_files("src/*.cc")
    add_files("src/*.rc")
    -- The standalone updater exe (chrome_green_updater target) is embedded as
    -- RCDATA 1001 via src/updater_res.rc, which that target's after_build
    -- regenerates to point at the freshly linked exe.
    -- LZMA SDK (git submodule) — only the .c files needed for 7z extraction
    add_files(
        "7zip/C/7zArcIn.c",
        "7zip/C/7zDec.c",
        "7zip/C/7zAlloc.c",
        "7zip/C/7zBuf.c",
        "7zip/C/7zCrc.c",
        "7zip/C/7zCrcOpt.c",
        "7zip/C/7zStream.c",
        "7zip/C/Alloc.c",
        "7zip/C/Bcj2.c",
        "7zip/C/Bra.c",
        "7zip/C/Bra86.c",
        "7zip/C/BraIA64.c",
        "7zip/C/CpuArch.c",
        "7zip/C/Delta.c",
        "7zip/C/LzmaDec.c",
        "7zip/C/Lzma2Dec.c",
        "7zip/C/Ppmd7.c",
        "7zip/C/Ppmd7aDec.c",
        "7zip/C/Ppmd7Dec.c",
        "7zip/C/Sha256.c",
        "7zip/C/Sha256Opt.c"
    )
    add_includedirs("src", "7zip/C")
    -- NOTE: do NOT force "_AMD64_" here. It is a Windows SDK platform macro;
    -- defining it on x86/arm64 breaks winbase.h/winnt.h Interlocked intrinsic
    -- macros (InterlockedIncrement64 etc. become undeclared under clang-cl),
    -- and 7z already detects the arch via the compiler-defined _M_AMD64/_M_IX86/
    -- _M_ARM64. Only Z7_EXTRACT_ONLY / _CRT_SECURE_NO_WARNINGS are needed.
    add_defines("Z7_EXTRACT_ONLY", "_CRT_SECURE_NO_WARNINGS")
    add_links("onecore", "propsys", "oleacc", "winhttp", "ws2_32", "bcrypt", "shlwapi", "version", "netapi32", "ole32", "oleaut32", "MMDevApi", "uuid", "UIAutomationCore")
    if is_mode("release") then
        -- arm64 has no vc-ltl5 prebuilt; rely on the MSVC static runtime instead.
        if not is_arch("arm64") then
            add_packages("vc-ltl5")
        end
    end
    if is_arch("arm64") then
        -- clang-cl cross-compiling to arm64 does not feed the ARM64 MSVC CRT lib
        -- dir (libcmt.lib / libcpmt.lib / oldnames.lib) to lld-link when vc-ltl5 is
        -- excluded, so point the linker at it explicitly via add_linkdirs (xmake
        -- emits the proper /libpath: for clang-cl/lld-link). A concrete, already
        -- verified path is used as a guaranteed fallback (os.isdir), with glob
        -- patterns for version-independence; the BuildTools path has no spaces.
        local tried = {}
        local function add(dir)
            dir = (dir or ""):gsub("\\", "/")
            if dir == "" or tried[dir] then return end
            tried[dir] = true
            if os.isdir(dir) then
                add_linkdirs(dir)
            end
        end
        -- concrete known path (verified on this machine) — guaranteed fallback
        add("C:/BuildTools/VC/Tools/MSVC/14.44.35207/lib/ARM64")
        -- glob for version-independent discovery
        for _, pat in ipairs({
            "C:/BuildTools/VC/Tools/MSVC/*/lib/ARM64",
            "C:/Program Files (x86)/Microsoft Visual Studio/*/VC/Tools/MSVC/*/lib/ARM64",
            "C:/Program Files/Microsoft Visual Studio/*/VC/Tools/MSVC/*/lib/ARM64",
        }) do
            for _, d in ipairs(os.dirs(pat) or {}) do add(d) end
        end
    end
    after_build(function (target)
        if is_mode("release") then
            for _, file in ipairs(os.files("$(builddir)/release/*")) do
                if not file:endswith("dll") then
                    os.rm(file)
                end
            end
        end
    end)