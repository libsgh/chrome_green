@echo off
setlocal EnableDelayedExpansion

REM Resolve project root = parent of this script's directory (scripts\..)
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "PROJECT_ROOT=%SCRIPT_DIR%\.."

REM Lock CWD to project root regardless of caller's directory
pushd "%PROJECT_ROOT%" || (echo [build.bat] cannot cd to %PROJECT_ROOT% & exit /b 1)

REM ============================================================
REM  CONFIG - edit these for your local debug build
REM ============================================================
REM Path to the clang-cl + LLVM toolchain bin directory. Other devs
REM only need to change this one line to point at their own install.
set "CLANG_BIN=D:\Programs\clang+llvm-20.1.8-x86_64-pc-windows-msvc\bin"

REM Directory of your TEST Chrome (the one this script kills + relaunches
REM on a plain no-arg build). NEVER set this to your daily Chrome - the
REM deploy step only ever touches Chrome living under this path.
set "CHROME_TEST_DIR=D:\Programs\chrome_test\App"
REM ============================================================

REM ---- Target platform ----
REM   (no arg)      x64 only, then deploy to chrome_test + auto-launch (legacy behavior)
REM   x86|x64|arm64 build that single arch; version.dll lands at build\release (no deploy)
REM   all           build x86+x64+arm64, then package build\release\ChromeGreen_vX.Y.Z.zip
REM                with nested x86/x64\version.dll (no App/Data/Cache, no ini)
REM   updater       build ONLY chrome_green_updater.exe (no web build, no dll, no deploy).
REM                Optionally appends an arch (x86|x64|arm64, default x64) and/or "test"
REM                to auto-launch the standalone preview window (--test). Examples:
REM                  build.bat updater
REM                  build.bat updater x86
REM                  build.bat updater test
REM                  build.bat updater x64 test
set "HAD_ARG=0"
set "TARGET=%~1"
if "%TARGET%"=="" (set "TARGET=x64") else (set "HAD_ARG=1")

REM ---- updater-only quick build (early return, skips web + dll) ----
if /I "%TARGET%"=="updater" (
  REM Kill any previously launched preview window first — it holds the exe
  REM open and a rebuild would fail with "permission denied", leaving the
  REM user staring at the stale (pre-fix) window. Harmless if none running.
  taskkill /F /IM chrome_green_updater.exe >nul 2>&1
  set "ARCH=%~2"
  set "RUN_TEST=0"
  if /I "!ARCH!"=="test" (set "ARCH=x64" & set "RUN_TEST=1")
  if /I "%~3"=="test" set "RUN_TEST=1"
  if "!ARCH!"=="" set "ARCH=x64"
  echo [build.bat] building chrome_green_updater.exe only ^(!ARCH!^) ...
  xmake f -p windows -m release -a !ARCH! --toolchain=clang-cl --bin="%CLANG_BIN%" --yes
  if errorlevel 1 (echo [build.bat] xmake f failed & exit /b 1)
  xmake build -r chrome_green_updater
  if errorlevel 1 (echo [build.bat] updater build failed & exit /b 1)
  echo [build.bat] done: build\release\chrome_green_updater.exe
  if "!RUN_TEST!"=="1" (
    echo [build.bat] launching preview window (--test^) ...
    start "" "build\release\chrome_green_updater.exe" --test
  )
  popd
  endlocal
  exit /b 0
)

REM ---- Build the Vue config page and regenerate src/web_content.h ----
REM Set SKIP_WEB=1 to skip this (e.g. for pure C++ iterations) when web/dist
REM is already current. Requires node/npm on PATH.
if not defined SKIP_WEB (
  echo [build.bat] building web config page ...
  pushd web
  call npm run build
  if errorlevel 1 (
    popd
    echo [build.bat] web build failed, aborting.
    exit /b 1
  )
  popd
  node scripts/embed.mjs
  if errorlevel 1 (
    echo [build.bat] embed failed, aborting.
    exit /b 1
  )
) else (
  echo [build.bat] SKIP_WEB set, skipping web build.
)

if /I "%TARGET%"=="all" (
  set "ARCHES=x86 x64 arm64"
) else (
  set "ARCHES=%TARGET%"
)

REM ---- Version string (for the release zip name) from src/version.h ----
REM /c: keeps the pattern as one literal (otherwise the space splits it into
REM two patterns and "#define RELEASE_VER_STR \" also matches, yielding garbage).
set "VM=0" & set "VS=0" & set "VF=0"
for /f "tokens=3" %%a in ('findstr /b /c:"#define RELEASE_VER_MAIN" src\version.h') do set "VM=%%a"
for /f "tokens=3" %%a in ('findstr /b /c:"#define RELEASE_VER_SUB" src\version.h') do set "VS=%%a"
for /f "tokens=3" %%a in ('findstr /b /c:"#define RELEASE_VER_FIX" src\version.h') do set "VF=%%a"
set "VER=%VM%.%VS%.%VF%"

REM ---- For "all", stage each arch directly under build\release (x86/x64/arm64) ----
if /I "%TARGET%"=="all" (
  for %%D in (x86 x64 arm64) do (
    if exist "build\release\%%D" rmdir /S /Q "build\release\%%D"
  )
)

REM ---- Build each requested architecture (two-step: updater exe then dll) ----
for %%A in (%ARCHES%) do (
  echo [build.bat] configuring + building for %%A ...
  REM -p windows is required: on some setups xmake auto-detects "mingw" for
  REM x86 and then rejects vc-ltl5 ("unsupported on mingw/x86"). Pinning the
  REM platform avoids that misdetection for every arch.
  xmake f -p windows -m release -a %%A --toolchain=clang-cl --bin="%CLANG_BIN%" --yes
  if errorlevel 1 (
    echo [build.bat] xmake f failed for %%A
    exit /b 1
  )

  REM Step 1: force-rebuild the standalone updater exe so its after_build
  REM regenerates src/updater_res.rc (embeds the exe into version.dll as RCDATA
  REM 1001). Must run before the dll build on a clean tree.
  xmake build -r chrome_green_updater
  if errorlevel 1 (
    echo [build.bat] updater build failed for %%A
    exit /b 1
  )

  REM Step 2: build the full dll, which embeds the exe via the rc above.
  xmake build
  if errorlevel 1 (
    echo [build.bat] dll build failed for %%A
    exit /b 1
  )

  if exist "build\release\version.dll" (
    if /I "%TARGET%"=="all" (
      if not exist "build\release\%%A" mkdir "build\release\%%A"
      copy /Y "build\release\version.dll" "build\release\%%A\version.dll"
      echo [build.bat] copied version.dll for %%A into build\release\%%A
    ) else (
      echo [build.bat] version.dll for %%A is at build\release\version.dll
    )
  ) else (
    echo [build.bat] version.dll missing for %%A, build may have failed.
    exit /b 1
  )
)

if /I "%TARGET%"=="all" (
  REM ---- Package a release zip into build\release with per-arch folders ----
  REM No App/Data/Cache nesting, no chrome_green.ini (generated at runtime).
  echo [build.bat] packaging build\release\ChromeGreen_v%VER%.zip ...
  tar -a -c -f "build\release\ChromeGreen_v%VER%.zip" -C "build\release" x86 x64 arm64
  if errorlevel 1 (
    echo [build.bat] zip creation failed.
    exit /b 1
  )
  echo [build.bat] created build\release\ChromeGreen_v%VER%.zip with:
  tar -tf "build\release\ChromeGreen_v%VER%.zip"
  REM Staging subdirs (x86/x64/arm64) are only needed for packaging; drop them now.
  for %%D in (x86 x64 arm64) do (
    if exist "build\release\%%D" rmdir /S /Q "build\release\%%D"
  )
) else (
  if "%HAD_ARG%"=="1" (
    REM ---- Explicit single-arch target: just leave version.dll in build\release, no deploy ----
    echo [build.bat] done. version.dll is at build\release\version.dll
  ) else (
    REM ---- Legacy no-arg: deploy x64 to chrome_test + auto-launch ----
    if exist "build\release\version.dll" (
      echo [build.bat] version.dll generated, deploying to chrome_test ...
      REM Only terminate the TEST Chrome under CHROME_TEST_DIR. NEVER kill the daily
      REM Chrome pointed at by your desktop shortcut. taskkill's IMAGEPATH filter is
      REM unreliable on some Windows builds (it just errors out), so we kill by exact
      REM executable path via PowerShell, which is built into Windows 10/11.
      powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-Process -Name chrome -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '%CHROME_TEST_DIR%\chrome.exe' } | Stop-Process -Force" >nul 2>&1
      REM Wait for the process to release the lock. Use ping, NOT the Windows
      REM 'timeout' builtin: under Git Bash, GNU coreutils' timeout shadows it and
      REM breaks the wait (so the copy below would fail with "file in use").
      ping -n 2 127.0.0.1 >nul 2>&1
      REM Copy version.dll, retrying a few times in case the lock is held
      REM briefly after the kill above. Kept inline (no goto/subroutine) to
      REM avoid cmd scoping pitfalls.
      set "DEPLOY_OK=0"
      for /L %%R in (1,1,6) do (
        if "!DEPLOY_OK!"=="0" (
          copy /Y "build\release\version.dll" "%CHROME_TEST_DIR%\version.dll" >nul 2>&1
          if not errorlevel 1 set "DEPLOY_OK=1"
        )
      )
      if "!DEPLOY_OK!"=="1" (
        echo [build.bat] deployed to %CHROME_TEST_DIR%\version.dll
        start "" "%CHROME_TEST_DIR%\chrome.exe"
      ) else (
        echo [build.bat] deploy copy failed - close the test Chrome, then re-run.
      )
    ) else (
      echo [build.bat] version.dll not found, build may have failed.
    )
  )
)

popd
endlocal
exit /b 0
