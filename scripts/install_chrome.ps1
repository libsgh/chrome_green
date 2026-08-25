<#
  Portable Chrome Online Install

  Install / update a portable Chrome on Windows and apply a
  portability plugin (chrome_green or chrome_plus).

  Usage (matches chrome_green's update / download channels):

      irm chrome.noki.eu.org | iex

  Or run locally:

      powershell -ExecutionPolicy Bypass -File install_chrome.ps1

  Fully self-contained. At runtime it:
    1. Prompts for the install directory (default D:\Program Files\Chrome)
    2. Picks an update channel (stable / beta / dev / canary, default stable)
    3. Picks a download channel (edgedl / dl.google.com / www.google.com / redirector, default dl.google.com)
    4. Picks a portability plugin (1=chrome_green / 2=chrome_plus, default chrome_green)
    5. Shows a live progress bar while downloading and extracting
    6. Saves the configuration and opens the install directory

  Notes:
    - Chrome is fetched via the Google Omaha protocol (same source as chrome_green),
      and the CDN host is chosen from the selected download channel.
    - Extraction uses the standalone 7-Zip (7za.exe), downloaded to a temp dir at
      runtime so the install directory is never polluted.
    - The plugin's version.dll is placed next to chrome.exe to enable portability.
    - On first install, App/Data/Cache directories are created. On update, the
      saved configuration is offered for confirmation; temp files are always cleaned.

  Uninstall (remove everything this tool created):

      powershell -ExecutionPolicy Bypass -File install_chrome.ps1 -Uninstall
      powershell -ExecutionPolicy Bypass -File install_chrome.ps1 -Uninstall -Force

  Or pick "Uninstall" from the action menu when running interactively.
  -Force skips the confirmation prompt (for scripting).
#>

param(
    [switch]$Uninstall,
    [switch]$Force
)

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
$ErrorActionPreference = 'Stop'

# --- Script metadata ---
$ScriptDate = '2026-08-20'

# --- Configurable mirrors / endpoints (edit these to change sources) ---
# GitHub proxy mirror prefix. BOTH the pre-baked release-index JSON
# (libsgh/ghapi-json-generator raw) AND the plugin archive downloads are routed
# through this prefix, so a single setting proxies every GitHub fetch.
# Leave empty ('') to hit GitHub directly (no mirror).
$GhMirror = 'http://gh.noki.eu.org'

# Chrome download CDN hosts. Order = menu order. Pick a mirror/proxy host here
# if you want to redirect all Chrome downloads through it.
$DownloadChannels = @(
    @{ Host = 'edgedl.me.gvt1.com';   Desc = 'edgedl.me.gvt1.com' },
    @{ Host = 'dl.google.com';        Desc = 'dl.google.com' },
    @{ Host = 'www.google.com';       Desc = 'www.google.com' },
    @{ Host = 'redirector.gvt1.com';  Desc = 'redirector.gvt1.com' }
)
# Default download channel index into $DownloadChannels (1-based). 2 = dl.google.com.
$DownloadDefault = 2

# ASCII art shown at startup (renders "CHROME").
$BannerArt = @'
 ,-----.,--.                                    
'  .--./|  ,---. ,--.--. ,---. ,--,--,--.,---.  
|  |    |  .-.  ||  .--'| .-. ||        | .-. : 
'  '--'\|  | |  ||  |   ' '-' '|  |  |  \   --. 
 `-----'`--' `--'`--'    `---' `--`--`--'`----' 
'@

# --- Base helpers ---

function Write-Banner($msg) {
    Write-Host ""
    Write-Host ("=" * 60) -ForegroundColor DarkCyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host ("=" * 60) -ForegroundColor DarkCyan
}

function Write-Step($msg) {
    Write-Host ""
    Write-Host ">> $msg" -ForegroundColor Green
}

function Write-Status($msg) {
    Write-Host "   $msg" -ForegroundColor Gray
}

# Detect the native OS architecture. Uses PROCESSOR_ARCHITEW6432 (only set
# when a 32-bit process runs on a 64-bit OS) so it distinguishes AMD64 from
# ARM64 — unlike Is64BitOperatingSystem which collapses both to "64-bit".
function Get-SystemArchitecture {
    $raw = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    switch ($raw) {
        'ARM64' { return 'arm64' }
        'AMD64' { return 'x64' }
        'x86'   { return 'x86' }
        default { return 'x64' }
    }
}

# Interactive menu: arrow keys move the highlight, Enter confirms,
# optional digit keys (1-9) jump and confirm, Esc cancels.
# Uses relative ANSI cursor moves (no absolute coordinates) so console
# scrolling can never break repositioning.
function Read-Menu($Title, $Options, $DefaultIndex) {
    Write-Host ""
    Write-Host "== $Title ==" -ForegroundColor Cyan
    $defIdx = if ($DefaultIndex -ge 1 -and $DefaultIndex -le $Options.Count) { $DefaultIndex - 1 } else { 0 }
    $sel = $defIdx
    $esc = [char]27
    $hide = $esc + "[?25l"
    $show = $esc + "[?25h"
    $rewrite = $esc + "[" + ($Options.Count) + "A" + $esc + "[J"

    function Render {
        for ($i = 0; $i -lt $Options.Count; $i++) {
            $txt = $Options[$i]
            if ($i -eq $sel) {
                Write-Host ("  > " + $txt) -BackgroundColor DarkCyan -ForegroundColor White
            } else {
                Write-Host ("    " + $txt)
            }
        }
    }

    [Console]::Write($hide)
    Render

    while ($true) {
        $key = [Console]::ReadKey($true)
        $k = $key.Key
        if ($k -eq 'UpArrow') {
            $sel = if ($sel -le 0) { $Options.Count - 1 } else { $sel - 1 }
            [Console]::Write($rewrite); Render
        } elseif ($k -eq 'DownArrow') {
            $sel = if ($sel -ge $Options.Count - 1) { 0 } else { $sel + 1 }
            [Console]::Write($rewrite); Render
        } elseif ($k -eq 'Enter') {
            [Console]::Write($show)
            return $sel + 1
        } elseif ($k -eq 'Escape') {
            [Console]::Write($show)
            return $null
        } elseif ($k -ge 'D1' -and $k -le 'D9') {
            $n = [int]$k - [int]'D1' + 1
            if ($n -ge 1 -and $n -le $Options.Count) {
                [Console]::Write($show)
                return $n
            }
        }
    }
}

# Download a file. Prefers curl.exe (built-in, HTTP/2) for browser-like
# speed; falls back to System.Net if curl is unavailable.
# Shows an INLINE progress bar directly under the step header (not Write-Progress).
function Invoke-DownloadFile($Url, $Dest, $Activity) {
    $esc = [char]27

    # Render a single-line progress bar in-place (cursor-up + clear-line).
    function Show-Bar($doneBytes, $totalBytes) {
        $w = try { [Console]::WindowWidth } catch { 60 }
        if ($w -lt 30) { $w = 60 }
        $barW = $w - 32
        if ($totalBytes -gt 0) {
            $pct = [int]($doneBytes * 100 / $totalBytes)
            $fill = [int]([math]::Min($pct * $barW / 100, $barW))
            $bar = ('#' * $fill) + ('-' * ($barW - $fill))
            $dMB = [math]::Round($doneBytes / 1MB, 1); $tMB = [math]::Round($totalBytes / 1MB, 1)
            $txt = ("   [{0}] {1,3}%  {2}MB / {3}MB" -f $bar, $pct, $dMB, $tMB)
        } else {
            $dMB = [math]::Round($doneBytes / 1MB, 1)
            $txt = ("   Downloaded {0}MB..." -f $dMB)
        }
        [Console]::Write(($esc + "[1A" + $esc + "[2K" + $txt + "`n"))
    }

    # Print a placeholder so the first Show-Bar has a line to overwrite.
    Write-Host "   ..."
    $useCurl = $false

# --- Clean up / validate any existing file before (re)downloading ---
    # An interrupted run may have left a partial or a curl-locked file here.
    # Kill the locking curl, then verify completeness (HEAD vs on-disk size)
    # so we reuse only genuinely complete files and re-download partials.
    Stop-LockingCurl $Dest
    Start-Sleep -Milliseconds 200
    $existingFile = Get-Item -LiteralPath $Dest -ErrorAction SilentlyContinue
    $haveComplete = $false
    if ($existingFile -and -not $existingFile.PSIsContainer -and $existingFile.Length -gt 0) {
        $headTotal = 0
        try {
            $head = & curl.exe -sIL $Url 2>$null
            foreach ($line in $head) {
                if ($line -match '(?i)^content-length:\s*(\d+)') { $headTotal = [long]$Matches[1]; break }
            }
        } catch { }
        if ($headTotal -gt 0) {
            if ($existingFile.Length -eq $headTotal) { $haveComplete = $true }
        } else {
            # Size unknown (HEAD unsupported): trust a non-locked file.
            if (-not (Test-FileLocked $Dest)) { $haveComplete = $true }
        }
    }
    if ($haveComplete) {
        Write-Status ("Reusing cached file: {0}" -f $Dest)
        return
    }
    # Remove the partial (retry once after a brief wait in case it was locked).
    if ($existingFile) {
        try { Remove-Item -LiteralPath $Dest -Force -ErrorAction Stop }
        catch {
            Start-Sleep -Milliseconds 400
            try { Remove-Item -LiteralPath $Dest -Force -ErrorAction Stop } catch { }
        }
    }
    try { if (Get-Command curl.exe -ErrorAction SilentlyContinue) { $useCurl = $true } } catch { }

    if ($useCurl) {
        $total = 0
        try {
            $head = & curl.exe -sIL $Url 2>$null
            foreach ($line in $head) {
                if ($line -match '(?i)^content-length:\s*(\d+)') { $total = [long]$Matches[1]; break }
            }
        } catch { }

        # curl may finish the body but still report a non-zero exit (notably
        # exit 56 "failure receiving network data" when the server closes the
        # connection during teardown — common with HTTP/2 / some CDNs). So we
        # retry a few times and, crucially, treat a fully-written file as success
        # even if curl complains, instead of throwing away a complete download.
        $maxTries = 3
        $ok = $false
        for ($try = 1; $try -le $maxTries; $try++) {
            Stop-LockingCurl $Dest
            Start-Sleep -Milliseconds 200
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = 'curl.exe'
            $psi.Arguments = @('-L', '-o', $Dest, $Url)
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow = $true
            $p = [System.Diagnostics.Process]::Start($psi)
            while (-not $p.HasExited) {
                Start-Sleep -Milliseconds 250
                $done = 0
                if (Test-Path $Dest) { $done = (Get-Item $Dest).Length }
                Show-Bar $done $total
            }
            $p.WaitForExit()
            $finalDone = if (Test-Path $Dest) { (Get-Item $Dest).Length } else { 0 }
            Show-Bar $finalDone $total
            if ($p.ExitCode -eq 0) { $ok = $true; break }
            # File fully written despite the error: accept it.
            if ($total -gt 0 -and $finalDone -eq $total) {
                Write-Status ("curl exit $($p.ExitCode) but file complete ($finalDone bytes); accepting.")
                $ok = $true; break
            }
            # Size unknown (HEAD unsupported): a non-empty file is our best signal.
            if ($total -eq 0 -and $finalDone -gt 0) {
                Write-Status ("curl exit $($p.ExitCode) but non-empty file written; accepting.")
                $ok = $true; break
            }
            Write-Status ("curl exit $($p.ExitCode); download incomplete, retrying ($try/$maxTries)...")
            try { Remove-Item -LiteralPath $Dest -Force -ErrorAction SilentlyContinue } catch { }
            Start-Sleep -Milliseconds 500
        }
        if (-not $ok) { throw "curl.exe failed (exit $($p.ExitCode)) downloading $Url" }
        return
    }

    # Fallback: System.Net.HttpWebRequest (browser-like User-Agent, 1MB buffer).
    $req = [System.Net.HttpWebRequest]::Create($Url)
    $req.UserAgent = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36'
    $req.AllowAutoRedirect = $true
    $req.Method = 'GET'
    $req.Timeout = 600000
    $req.ReadWriteTimeout = 600000
    $resp = $req.GetResponse()
    $total = $resp.ContentLength
    $stream = $resp.GetResponseStream()
    $fs = [System.IO.File]::Create($Dest)
    $buf = New-Object byte[] 1048576
    $read = 0
    $done = 0
    try {
        while (($read = $stream.Read($buf, 0, $buf.Length)) -gt 0) {
            $fs.Write($buf, 0, $read)
            $done += $read
            Show-Bar $done $total
        }
    } finally {
        $fs.Close(); $stream.Close(); $resp.Close()
        Show-Bar $done $total
    }
}

# --- GitHub Release parsing ---

# Resolve the latest release metadata from a pre-baked static JSON generated by
# libsgh/ghapi-json-generator. It mirrors GET /repos/<owner>/<repo>/releases?per_page=10
# and is served from github.com raw (CDN-cached, quota-free) — replacing both the
# old /releases/latest 302 web trick and the 60/h-limited REST API.
#
# The JSON is an ARRAY of releases (newest first). We filter out draft /
# prerelease entries and pick the highest stable version, then return its tag
# and asset list. All GitHub fetches still go through $GhMirror so proxying is
# unchanged.
function Get-GitHubReleaseIndex($Repo) {
    $url = ("$GhMirror/github.com/libsgh/ghapi-json-generator/raw/refs/heads/output/v2/repos/{0}/releases%3Fper_page=10/data.json" -f $Repo)
    $req = [System.Net.HttpWebRequest]::Create($url)
    $req.UserAgent = 'PortableChromeInstaller'
    $req.Accept = 'application/json'
    $req.Method = 'GET'
    $req.Timeout = 20000
    $resp = $req.GetResponse()
    $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
    $json = $sr.ReadToEnd(); $sr.Close(); $resp.Close()
    $releases = $json | ConvertFrom-Json

    # Keep only published stable releases (skip draft / prerelease).
    $stable = $releases | Where-Object { -not $_.prerelease -and -not $_.draft }

    $best = $null
    foreach ($r in $stable) {
        if ($null -eq $best) { $best = $r; continue }
        # Compare-Version defaults to a three-way sign (-1/0/1); use PowerShell's
        # real -gt operator on that result. Passing "-gt 0" as an arg misbinds
        # $op and makes the function return the raw sign, which is always truthy.
        if ((Compare-Version $r.tag_name $best.tag_name) -gt 0) { $best = $r }
    }
    if ($null -eq $best) { return $null }
    return @{ Tag = $best.tag_name; Assets = $best.assets }
}

# Compare two version strings. Returns $true when ($a -op $b) holds, where
# -op is one of -gt / -lt / -eq. Uses the same numeric-only parts as the rest
# of the script. Defaults to a three-way: returns the sign (-1/0/1) when no
# operator is given.
function Compare-Version($a, $b, [string]$op) {
    $pa = Get-VersionParts $a
    $pb = Get-VersionParts $b
    if ($pa.Count -eq 0 -or $pb.Count -eq 0) { return $false }
    $n = [Math]::Min($pa.Count, $pb.Count)
    for ($i = 0; $i -lt $n; $i++) {
        if ($pa[$i] -ne $pb[$i]) {
            $c = if ($pa[$i] -gt $pb[$i]) { 1 } else { -1 }
            if (-not $op) { return $c }
            if ($op -eq '-gt') { return $c -gt 0 }
            if ($op -eq '-lt') { return $c -lt 0 }
            if ($op -eq '-eq') { return $c -eq 0 }
            return $false
        }
    }
    $c = if ($pa.Count -ne $pb.Count) {
        if ($pa.Count -gt $pb.Count) { 1 } else { -1 }
    } else { 0 }
    if (-not $op) { return $c }
    if ($op -eq '-gt') { return $c -gt 0 }
    if ($op -eq '-lt') { return $c -lt 0 }
    if ($op -eq '-eq') { return $c -eq 0 }
    return $false
}

# --- Config persistence ---

function Get-ExistingConfig($path) {
    if (Test-Path $path) {
        try { return (Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { return $null }
    }
    return $null
}

function Save-Config($path, $cfg) {
    $cfg | ConvertTo-Json | Set-Content -Path $path -Encoding UTF8
}

# Normalize a version string to a list of numeric components (ignores a
# leading 'v'/'V' and any non-numeric suffix) for loose equality checks.
function Get-VersionParts($v) {
    if ([string]::IsNullOrWhiteSpace($v)) { return @() }
    $s = $v.TrimStart('v', 'V')
    $parts = $s -split '[.\-+]' | Where-Object { $_ -match '^\d+$' }
    return @($parts | ForEach-Object { [int]$_ })
}

# Extract the embedded semantic version from a plugin asset filename.
# chrome_plus archives are named like Chrome++_v1.18.2_x86_x64_arm64.7z, so the
# authoritative version lives in the filename rather than the release tag.
function Get-VersionFromAssetName($name) {
    if ([string]::IsNullOrWhiteSpace($name)) { return $null }
    $m = [regex]::Match($name, 'v(\d+(?:\.\d+)+)')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

# Loose equality: equal if the shorter sequence of numeric parts matches.
# e.g. 1.0.1 == 1.0.1.0, and 131.0.6778.108 == 131.0.6778.108.
function Test-VersionEqual($a, $b) {
    $pa = Get-VersionParts $a
    $pb = Get-VersionParts $b
    if ($pa.Count -eq 0 -or $pb.Count -eq 0) { return $false }
    $n = [Math]::Min($pa.Count, $pb.Count)
    for ($i = 0; $i -lt $n; $i++) { if ($pa[$i] -ne $pb[$i]) { return $false } }
    return $true
}

function Get-InstalledFileVersion($path) {
    if (Test-Path $path) {
        try {
            $v = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileVersion
            if ($v) { return $v.Trim() }
        } catch { }
    }
    return $null
}

# Remove old Chrome version subdirectories (e.g. 151.0.7922.170, 154.0.8014.0)
# from the App directory that accumulate across updates.
function Remove-OldVersionDirs($dir) {
    if (-not (Test-Path $dir)) { return }
    # Match folders whose name looks like a Chrome version: N.N.N.N
    $oldDirs = Get-ChildItem -Path $dir -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' }
    foreach ($d in $oldDirs) {
        try {
            Remove-Item -Path $d.FullName -Recurse -Force -ErrorAction Stop
            Write-Status ("  Removed old version folder: {0}" -f $d.Name)
        } catch {
            Write-Host ("  [warning] Could not remove {0}: {1}" -f $d.Name, $_.Exception.Message) -ForegroundColor Yellow
        }
    }
}

# Kill any chrome.exe process running from the given App directory so that
# file copy/overwrite operations do not fail with "file in use" errors.
# Returns $true if a process was killed, $false otherwise.
function Ensure-NoChromeProcess($appDir) {
    $chromeExe = Join-Path $appDir 'chrome.exe'
    if (-not (Test-Path $chromeExe)) { return $false }
    $resolved = (Resolve-Path -Path $chromeExe -ErrorAction SilentlyContinue).Path
    if (-not $resolved) { return $false }
    $procs = Get-Process -Name 'chrome' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path -eq $resolved }
    if ($procs.Count -eq 0) { return $false }
    $count = $procs.Count
    Write-Host ""
    Write-Host ("  Found {0} chrome.exe instance(s) running from this installation." -f $count) -ForegroundColor Yellow
    Write-Host ("  Files are locked and cannot be updated while Chrome is running.") -ForegroundColor Yellow
    $confirm = Read-Host "  Force close Chrome to continue? [Y/n]"
    if ($confirm -match '^[Nn]') {
        throw "Update aborted: Chrome is still running and files are locked."
    }
    $killed = 0
    foreach ($p in $procs) {
        try {
            $p.Kill()
            $p.WaitForExit(5000) | Out-Null
            $killed++
        } catch {
            Write-Host ("  [warning] Failed to kill PID {0}: {1}" -f $p.Id, $_.Exception.Message) -ForegroundColor Yellow
        }
    }
    # Brief pause for OS to release file handles
    Start-Sleep -Milliseconds 500
    Write-Status ("  Closed {0}/{1} Chrome process(es)." -f $killed, $count)
    return ($killed -gt 0)
}

# Fully remove everything this tool created:
#   - install directory (App / Data / Cache)
#   - desktop shortcuts that point at this install's chrome.exe
#   - the temp cache directory (installers, 7za, plugin archives, config)
function Invoke-Uninstall {
    param([switch]$Force)

    $cacheDir = Join-Path $env:TEMP 'portable_chrome_install'
    $configFile = Join-Path $cacheDir 'chrome_green_install_config.json'
    $existing = Get-ExistingConfig $configFile

    # Resolve the install directory to remove.
    $installDir = $null
    if ($existing -and $existing.installDir) {
        $installDir = [System.IO.Path]::GetFullPath($existing.installDir)
    } else {
        $defaultDir = "D:\Program Files\Chrome"
        Write-Host ""
        Write-Host "== Uninstall: install directory ==" -ForegroundColor Cyan
        $dirInput = Read-Host ("  Enter the install directory to remove, Enter for default ({0})" -f $defaultDir)
        $installDir = if ([string]::IsNullOrWhiteSpace($dirInput)) { $defaultDir } else { $dirInput.Trim().Trim('"') }
        $installDir = [System.IO.Path]::GetFullPath($installDir)
    }
    $appDir = Join-Path $installDir 'App'
    $chromeTarget = Join-Path $appDir 'chrome.exe'

    # Show exactly what will be removed.
    Write-Host ""
    Write-Host "The following will be removed:" -ForegroundColor Yellow
    Write-Host ("  - Install directory (App/Data/Cache): $installDir")
    Write-Host ("  - Desktop shortcuts targeting: $chromeTarget")
    Write-Host ("  - Temp cache directory: $cacheDir")

    if (-not $Force) {
        $confirm = Read-Host "Proceed with uninstall? [N/y]"
        if ($confirm -notmatch '^[Yy]') {
            Write-Host "Uninstall cancelled." -ForegroundColor Yellow
            return
        }
    }

    # 1) Desktop shortcuts that point at this install's chrome.exe.
    #    Only our own shortcuts are removed; a genuine Google Chrome .lnk that
    #    targets a different path is left untouched.
    Write-Step "Removing desktop shortcuts"
    try {
        $shell = New-Object -ComObject WScript.Shell
        $desktop = [Environment]::GetFolderPath('Desktop')
        $wanted = $chromeTarget.Replace('/', '\').ToLower()
        Get-ChildItem $desktop -Filter '*.lnk' -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $lnk = $shell.CreateShortcut($_.FullName)
                $tgt = if ($lnk.TargetPath) { $lnk.TargetPath.Replace('/', '\').ToLower() } else { '' }
                [System.Runtime.InteropServices.Marshal]::ReleaseComObject($lnk) | Out-Null
                if ($tgt -eq $wanted) {
                    Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
                    Write-Status ("  Removed shortcut: {0}" -f $_.Name)
                }
            } catch { }
        }
        [System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell) | Out-Null
    } catch {
        Write-Host ("  [warning] Could not enumerate desktop shortcuts: {0}" -f $_.Exception.Message) -ForegroundColor Yellow
    }

    # 2) Close any running Chrome from this install so files can be deleted.
    Ensure-NoChromeProcess $appDir | Out-Null

    # 3) Install directory.
    Write-Step "Removing install directory"
    if (Test-Path $installDir) {
        try {
            Remove-Item $installDir -Recurse -Force -ErrorAction Stop
            Write-Status ("  Removed: {0}" -f $installDir)
        } catch {
            Write-Host ("  [warning] Could not fully remove {0}: {1}" -f $installDir, $_.Exception.Message) -ForegroundColor Yellow
        }
    } else {
        Write-Status "  Install directory not found (already gone)."
    }

    # 4) Temp cache directory (installers, 7za, plugin archives, config).
    Write-Step "Removing temp cache directory"
    if (Test-Path $cacheDir) {
        try {
            Remove-Item $cacheDir -Recurse -Force -ErrorAction Stop
            Write-Status ("  Removed: {0}" -f $cacheDir)
        } catch {
            Write-Host ("  [warning] Could not fully remove {0}: {1}" -f $cacheDir, $_.Exception.Message) -ForegroundColor Yellow
        }
    } else {
        Write-Status "  Temp cache not found (already gone)."
    }

    Write-Host ""
    Write-Host "Uninstall complete." -ForegroundColor Green
}

# --- Main flow ---

# --- Startup banner ---
Write-Host $BannerArt
Write-Host "  Welcome to the Portable Chrome Online Install tool" -ForegroundColor Cyan
Write-Host ("  Script date: {0}" -f $ScriptDate) -ForegroundColor Gray

# Fixed cache directory. All downloaded artifacts (Chrome installer, 7-Zip,
# plugin archives) and the saved config live here so they survive across runs
# and are NOT re-downloaded next time. Only intermediate extraction folders
# are cleaned after each run.
$cacheDir = Join-Path $env:TEMP 'portable_chrome_install'
New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null

# --- Helpers: free downloads locked by orphaned curl processes ---
# When the script is interrupted mid-download, the detached curl.exe child
# keeps running and holds an exclusive lock on the partial file. That blocks
# manual deletion and prevents the script from overwriting it. These helpers
# find and terminate such curl processes, and probe for locked files.
function Stop-LockingCurl($Path) {
    try {
        $procs = Get-CimInstance Win32_Process -Filter "Name = 'curl.exe'" -ErrorAction SilentlyContinue
        if (-not $procs) { return }
        $target = $Path.Replace('/', '\').ToLower()
        foreach ($pr in $procs) {
            $cmd = $pr.CommandLine
            if ($cmd -and $cmd.Replace('/', '\').ToLower().Contains($target)) {
                try { $pr | Invoke-CimMethod -MethodName Terminate -ErrorAction SilentlyContinue } catch { }
            }
        }
    } catch { }
}

function Test-FileLocked($Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', [System.IO.FileShare]::None)
        $fs.Close()
        return $false
    } catch {
        return $true
    }
}

# Free any partial download left locked by a curl orphan from a previous run.
# This also unblocks manual deletion of a stuck file.
Stop-LockingCurl $cacheDir
Start-Sleep -Milliseconds 300

$configFile = Join-Path $cacheDir 'chrome_green_install_config.json'
# Backward-compat: move a config left in flat TEMP into the cache dir.
$legacyConfig = Join-Path $env:TEMP 'chrome_green_install_config.json'
if (-not (Test-Path $configFile) -and (Test-Path $legacyConfig)) {
    try { Move-Item $legacyConfig $configFile -Force } catch { }
}
$existing = Get-ExistingConfig $configFile

# --- Mode selection: uninstall (param or menu) vs install/update ---
$doUninstall = $false
if ($Uninstall) {
    $doUninstall = $true
} else {
    # Top-level action menu. Default is "Install / Update" so a plain Enter
    # proceeds exactly as before; "Uninstall" only appears when chosen.
    $modeIdx = Read-Menu 'Select action' @('Install / Update', 'Uninstall (remove everything)') 1
    if ($null -eq $modeIdx) { $modeIdx = 1 }
    if ($modeIdx -eq 2) { $doUninstall = $true }
}
if ($doUninstall) {
    Invoke-Uninstall -Force:$Force
    return
}

# 1) Install directory
#    When a saved config exists, reuse its recorded path directly — no prompt,
#    so a re-run never "starts from the install directory" again.
#    When there is no config (first ever run), ask for the directory.
if ($existing -and $existing.installDir) {
    $InstallDir = [System.IO.Path]::GetFullPath($existing.installDir)
} else {
    $defaultDir = "D:\Program Files\Chrome"
    Write-Host ""
    Write-Host "== Install directory ==" -ForegroundColor Cyan
    Write-Host "  Default: $defaultDir"
    $dirInput = Read-Host "  Enter install directory, Enter for default"
    $InstallDir = if ([string]::IsNullOrWhiteSpace($dirInput)) { $defaultDir } else { $dirInput.Trim().Trim('"') }
    $InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
}

$isFirst = -not (Test-Path $InstallDir) -or ((Get-ChildItem $InstallDir -Force | Measure-Object).Count -eq 0)

# 2) Channel / download / plugin definitions
$updateChannels = @(
    @{ Key = 'stable'; Desc = 'stable' },
    @{ Key = 'beta';   Desc = 'beta' },
    @{ Key = 'dev';    Desc = 'dev' },
    @{ Key = 'canary'; Desc = 'canary' }
)
$plugins = @(
    @{ Key = 'chrome_green'; Repo = 'libsgh/chrome_green';  Desc = 'chrome_green' },
    @{ Key = 'chrome_plus';  Repo = 'Bush2021/chrome_plus'; Desc = 'chrome_plus' }
)

# 3) Defaults come from the saved config when present
$chDefault = 1; $dlDefault = $DownloadDefault; $plDefault = 1
if ($existing) {
    $ci = [array]::IndexOf($updateChannels.Key, $existing.channel); if ($ci -ge 0) { $chDefault = $ci + 1 }
    $di = [array]::IndexOf($DownloadChannels.Host, $existing.download); if ($di -ge 0) { $dlDefault = $di + 1 }
    $pi = [array]::IndexOf($plugins.Key, $existing.plugin); if ($pi -ge 0) { $plDefault = $pi + 1 }
}

# 4) Update-mode decision. If a saved config exists and the install dir is
#    populated and matches the recorded path, offer to update directly;
#    otherwise (or if declined) fall through to the menus, where the saved
#    values are shown as the highlighted defaults.
$savedPlugin = $null
$dirMatches = $false
if ($existing -and -not $isFirst) {
    $savedPlugin = $plugins | Where-Object { $_.Key -eq $existing.plugin }
    $dirMatches = (-not $existing.installDir) -or ($existing.installDir -eq $InstallDir)
}

# Resolve currently installed versions for display (read from disk first,
# fall back to saved config). These are shown next to Channel / Plugin.
$savedDir = if ($existing -and $existing.installDir) { $existing.installDir } else { $InstallDir }
$savedAppDir = Join-Path $savedDir 'App'
$installedChromeVer = Get-InstalledFileVersion (Join-Path $savedAppDir 'chrome.exe')
if (-not $installedChromeVer -and $existing -and $existing.version) {
    $installedChromeVer = $existing.version
}
$installedPluginVer = Get-InstalledFileVersion (Join-Path $savedAppDir 'version.dll')
if (-not $installedPluginVer -and $existing -and $existing.pluginVersion) {
    $installedPluginVer = $existing.pluginVersion
}

$skipMenus = $false
if ($savedPlugin -and $dirMatches) {
    Write-Banner "Update existing installation"
    Write-Host ""
    Write-Host "Saved configuration:" -ForegroundColor Cyan
    Write-Host "  Install dir : $($existing.installDir)"
    $chVer = if ($installedChromeVer) { ("  ({0})" -f $installedChromeVer) } else { '' }
    Write-Host "  Channel     : $($existing.channel)$chVer"
    Write-Host "  Download    : $($existing.download)"
    $plVer = if ($installedPluginVer) { ("  ({0})" -f $installedPluginVer) } else { '' }
    Write-Host "  Plugin      : $($existing.plugin)$plVer"
    Write-Host "  Saved at    : $configFile" -ForegroundColor Gray
    $ans = Read-Host "Update directly with this configuration? [Y/n]"
    if ($ans -notmatch '^[Nn]') {
        $channelKey = $existing.channel
        $dlHost = $existing.download
        $plugin = $savedPlugin
        $skipMenus = $true
    }
}

if ($isFirst) {
    Write-Banner "First installation"
} elseif (-not $skipMenus) {
    Write-Banner "Update existing installation"
    if ($existing -and -not $isFirst) {
        Write-Host ""
        Write-Host "Reconfiguring from scratch (saved values shown as defaults)." -ForegroundColor Yellow
    } else {
        Write-Host ""
        Write-Host "No saved configuration; please configure from scratch." -ForegroundColor Yellow
    }
}

if (-not $skipMenus) {
    # When reconfiguring an existing install, allow changing the location too.
    if ($existing -and -not $isFirst) {
        $defaultDir = $InstallDir
        Write-Host ""
        Write-Host "== Install directory ==" -ForegroundColor Cyan
        Write-Host "  Current: $defaultDir"
        $dirInput = Read-Host "  Enter new install directory, Enter to keep"
        if (-not [string]::IsNullOrWhiteSpace($dirInput)) {
            $InstallDir = [System.IO.Path]::GetFullPath($dirInput.Trim().Trim('"'))
        }
    }
    # 3) Update channel
    $chIdx = Read-Menu 'Update channel' ($updateChannels | ForEach-Object { $_.Desc }) $chDefault
    if ($null -eq $chIdx) { Write-Host "Cancelled." -ForegroundColor Yellow; return }
    $channelKey = $updateChannels[$chIdx - 1].Key

    # 4) Download channel
    $dlIdx = Read-Menu 'Download channel' ($DownloadChannels | ForEach-Object { $_.Desc }) $dlDefault
    if ($null -eq $dlIdx) { Write-Host "Cancelled." -ForegroundColor Yellow; return }
    $dlHost = $DownloadChannels[$dlIdx - 1].Host

    # 5) Portability plugin
    $plIdx = Read-Menu 'Portability plugin' ($plugins | ForEach-Object { $_.Desc }) $plDefault
    if ($null -eq $plIdx) { Write-Host "Cancelled." -ForegroundColor Yellow; return }
    $plugin = $plugins[$plIdx - 1]
}

# Continue confirmation (manual reconfigure / update path; the saved-config
# "update directly" path skips this).
if (-not $isFirst -and -not $skipMenus) {
    Write-Host ""
    $ov = Read-Host "This will update the installation (App directory). Continue? [Y/n]"
    if ($ov -match '^[Nn]') {
        Write-Host "Update cancelled." -ForegroundColor Yellow
        return
    }
}

# Prepare directories (App / Data / Cache) for portability.
$appDir = Join-Path $InstallDir 'App'
$dataDir = Join-Path $InstallDir 'Data'
$appCacheDir = Join-Path $InstallDir 'Cache'
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
New-Item -ItemType Directory -Path $appDir, $dataDir, $appCacheDir -Force | Out-Null
if ($isFirst) { Write-Status "Created directories: App, Data, Cache" }

# Detect system architecture early so it's available for the Configuration banner.
$sysArch = Get-SystemArchitecture

if (-not $skipMenus) {
    Write-Banner "Configuration"
    Write-Status ("Install dir : {0}" -f $InstallDir)
    Write-Status ("Arch        : {0}" -f $sysArch)
    $chVer = if ($installedChromeVer) { ("  ({0})" -f $installedChromeVer) } else { '' }
    Write-Status ("Channel     : {0}{1}" -f $channelKey, $chVer)
    Write-Status ("Download    : {0}" -f $dlHost)
    $plVer = if ($installedPluginVer) { ("  ({0})" -f $installedPluginVer) } else { '' }
    Write-Status ("Plugin      : {0}  ({1}){2}" -f $plugin.Key, $plugin.Repo, $plVer)
    Write-Host ""
} else {
    # Already displayed in the Saved configuration block above.
    Write-Status ("Arch        : {0}" -f $sysArch)
    Write-Host ""
}

# Cache preparation: the cache dir (created earlier) keeps every downloaded
#    artifact (Chrome installer, 7-Zip, plugin archives) so they are reused on
#    the next run. Intermediate extraction folders are cleared before/after use.
function Clear-ExtractDirs {
    foreach ($d in @('chrome_extract', 'plugin_extract')) {
        $p = Join-Path $cacheDir $d
        if (Test-Path $p) { try { Remove-Item -Path $p -Recurse -Force -ErrorAction SilentlyContinue } catch { } }
    }
}
Clear-ExtractDirs

# --- Prepare standalone 7-Zip (cached in the cache dir; reused across runs) ---
$sevenZip = Join-Path $cacheDir '7za.exe'
if (-not (Test-Path $sevenZip)) {
    Write-Step "Preparing 7-Zip extractor"
    $zip7 = Join-Path $cacheDir '7za920.zip'
    Invoke-DownloadFile 'https://www.7-zip.org/a/7za920.zip' $zip7 'Downloading 7-Zip (7za.exe)'
    Expand-Archive -Path $zip7 -DestinationPath $cacheDir -Force
    if (-not (Test-Path $sevenZip)) { throw "Failed to extract 7za.exe from 7za920.zip" }
    # Clean up files extracted from 7za920.zip that we don't need (keep only 7za.exe).
    @('7za920.zip', '7zxa.dll', '7-zip.chm', 'license.txt', 'readme.txt') | ForEach-Object {
        try { Remove-Item (Join-Path $cacheDir $_) -Force -ErrorAction SilentlyContinue } catch { }
    }
    Write-Status "7za.exe ready: $sevenZip"
}

# --- Step 1: query Omaha for the Chrome download URL ---
Write-Step "Querying Chrome version (Omaha, channel=$channelKey, arch=$sysArch)"
$appId = if ($channelKey -eq 'canary') { '{4EA16AC7-FD5A-47C3-875B-DBF4A2008C20}' } else { '{8A69D345-D564-463C-AFF1-A69D9E530F96}' }
$ap = "$sysArch-$channelKey-statsdef_1"
$omahaBody = @"
<?xml version="1.0" encoding="UTF-8"?>
<request protocol="3.0" updater="Omaha" updaterversion="1.3.36.152" shell_version="1.3.36.151" ismachine="0" sessionid="{11111111-1111-1111-1111-111111111111}" installsource="taggedmi" requestid="{11111111-1111-1111-1111-111111111111}" dedug="cr" domainjoined="0">
<hw physmemory="16" sse="1" sse2="1" sse3="1" ssse3="1" sse41="1" sse42="1" avx="1"/>
<os platform="win" version="10.0.22621.1028" sp="" arch="$sysArch"/>
<app appid="$appId" version="" nextversion="" ap="$ap" lang="en" brand="" client="" installage="-1" installdate="-1" iid="{11111111-1111-1111-1111-111111111111}">
	<updatecheck/>
	<data name="install" index="empty"/>
</app>
</request>
"@

$req = [System.Net.HttpWebRequest]::Create('https://tools.google.com/service/update2')
$req.Method = 'POST'
$req.ContentType = 'application/x-www-form-urlencoded'
$req.UserAgent = 'Google Update/1.3.36.152;winhttp'
$req.Timeout = 30000
$bytes = [System.Text.Encoding]::UTF8.GetBytes($omahaBody)
$req.ContentLength = $bytes.Length
$rs = $req.GetRequestStream(); $rs.Write($bytes, 0, $bytes.Length); $rs.Close()
$resp = $req.GetResponse()
$sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
$omahaXml = $sr.ReadToEnd(); $sr.Close(); $resp.Close()

$chromeVer = [regex]::Match($omahaXml, '<manifest version="([^"]+)"').Groups[1].Value
$pkgName = [regex]::Match($omahaXml, '<package[^>]*?name="([^"]+)"').Groups[1].Value
$codebases = [regex]::Matches($omahaXml, '<url codebase="([^"]+)"') | ForEach-Object { $_.Groups[1].Value }

if ([string]::IsNullOrWhiteSpace($chromeVer) -or [string]::IsNullOrWhiteSpace($pkgName) -or $codebases.Count -eq 0) {
    throw "Failed to parse the Omaha response (no version or download URL)."
}

# Pick the CDN host for the selected download channel (prefer https, fall back to first).
$chosen = $codebases | Where-Object { $_ -match ('https://' + [regex]::Escape($dlHost)) } | Select-Object -First 1
if (-not $chosen) { $chosen = $codebases | Where-Object { $_ -match [regex]::Escape($dlHost) } | Select-Object -First 1 }
if (-not $chosen) {
    Write-Status ("Download channel '$dlHost' not in Omaha's list, falling back to first CDN") -ForegroundColor Yellow
    $chosen = $codebases[0]
}
$chromeUrl = $chosen.TrimEnd('/') + '/' + $pkgName
Write-Status ("Chrome version : {0}" -f $chromeVer)
Write-Status ("Download URL   : {0}" -f $chromeUrl)

# --- Compare installed Chrome version with the latest from Omaha ---
$chromeExePath = Join-Path $appDir 'chrome.exe'
$installedChromeVer = Get-InstalledFileVersion $chromeExePath
$skipChrome = $false
if ($installedChromeVer -and (Test-VersionEqual $installedChromeVer $chromeVer)) {
    Write-Host ""
    Write-Host ("Chrome is already the latest version ($chromeVer).") -ForegroundColor Cyan
    $fc = Read-Host "Force reinstall Chrome anyway? [N/y]"
    if ($fc -notmatch '^[Yy]') { $skipChrome = $true }
}

# --- Step 2: download and extract Chrome (skipped when already up to date) ---
if (-not $skipChrome) {
    Write-Step "Downloading Chrome $chromeVer"
    $chromePkg = Join-Path $cacheDir ("chrome_" + $chromeVer + ".exe")
    # Keep only the current-version installer cached; drop stale ones.
    Get-ChildItem $cacheDir -Filter 'chrome_*.exe' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -ne $chromePkg } | ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue } catch { } }
    # Invoke-DownloadFile reuses a complete cached installer or re-downloads
    # (and deletes) a partial/locked one automatically.
    Invoke-DownloadFile $chromeUrl $chromePkg ("Downloading Chrome {0}" -f $chromeVer)

    Write-Step "Extracting Chrome package (two-stage 7z)"
    $chromeExtract = Join-Path $cacheDir 'chrome_extract'
    New-Item -ItemType Directory -Path $chromeExtract -Force | Out-Null
    & $sevenZip x $chromePkg -o"$chromeExtract" -y | Out-Null
    $inner7z = Join-Path $chromeExtract 'chrome.7z'
    if (Test-Path $inner7z) {
        & $sevenZip x $inner7z -o"$chromeExtract" -y | Out-Null
    }
    $chromeExe = Get-ChildItem $chromeExtract -Recurse -Filter chrome.exe | Select-Object -First 1
    if (-not $chromeExe) { throw "chrome.exe not found after extraction; the package may be incomplete." }
    $chromeRoot = $chromeExe.DirectoryName
    Write-Status ("Chrome extracted: {0}" -f $chromeRoot)

    # Clean up old version-named folders (e.g. 151.0.7922.170) left by
    # previous updates, and ensure no chrome.exe is locking files.
    Remove-OldVersionDirs $appDir
    Ensure-NoChromeProcess $appDir | Out-Null

    Write-Step "Installing Chrome into $appDir"
    Copy-Item (Join-Path $chromeRoot '*') $appDir -Recurse -Force
    Write-Status "Chrome files copied to App directory"

    # Free the extracted files; keep the cached installer for next time.
    Clear-ExtractDirs
} else {
    Write-Step "Chrome skipped (already up to date: $chromeVer)"
}

# --- Step 3: resolve, compare and (optionally) install the portability plugin ---
$pluginVersion = $null
$skipPlugin = $false
try {
    Write-Step "Resolving plugin release: $($plugin.Repo)"
    try { $rel = Get-GitHubReleaseIndex $plugin.Repo } catch {
        throw "Could not resolve the latest release for $($plugin.Repo): $_"
    }
    if (-not $rel -or -not $rel.Assets -or $rel.Assets.Count -eq 0) {
        throw "No stable release found for $($plugin.Repo) in the release index."
    }
    $tag = $rel.Tag

    $asset = $null
    if ($plugin.Key -eq 'chrome_green') {
        # Prefer a per-arch asset (e.g. ChromeGreen_v1.0.1_x64.zip) if present;
        # otherwise fall back to the combined archive (ChromeGreen_v1.0.1.zip).
        $cands = $rel.Assets | Where-Object { $_.name -match 'ChromeGreen_v.*\.zip$' }
        $asset = $cands | Where-Object { $_.name -match ('_' + $sysArch + '\.') } | Select-Object -First 1
        if (-not $asset) { $asset = $cands | Select-Object -First 1 }
        # chrome_green's tag_name matches its asset version (e.g. 2.0.0).
        $pluginVersion = $tag
    } else {
        # chrome_plus ships a single combined 7z (_x86_x64_arm64) with per-arch folders.
        $asset = $rel.Assets | Where-Object { $_.name -match 'Chrome\+\+_v.*_x86_x64_arm64\.7z$' } | Select-Object -First 1
        if (-not $asset) { $asset = $rel.Assets | Where-Object { $_.name -match 'Chrome\+\+.*\.7z$' } | Select-Object -First 1 }
        # The authoritative version for chrome_plus lives in the asset filename
        # (e.g. Chrome++_v1.18.2_x86_x64_arm64.7z), which may diverge from the
        # release tag. Prefer it so the reported version matches the download.
        $assetVer = Get-VersionFromAssetName $asset.name
        $pluginVersion = if ($assetVer) { $assetVer } else { $tag }
    }
    if (-not $asset) { throw "No matching plugin archive found in the $($plugin.Repo) release." }
    Write-Status ("Plugin version : {0}" -f $pluginVersion)

    # Compare installed plugin version (binary version.dll, else saved config).
    $verDllPath = Join-Path $appDir 'version.dll'
    $installedPluginVer = Get-InstalledFileVersion $verDllPath
    if (-not $installedPluginVer -and $existing -and $existing.pluginVersion) {
        $installedPluginVer = $existing.pluginVersion
    }
    if ($installedPluginVer -and (Test-VersionEqual $installedPluginVer $pluginVersion)) {
        Write-Host ""
        Write-Host ("Plugin $($plugin.Key) is already the latest version ($pluginVersion).") -ForegroundColor Cyan
        $fp = Read-Host "Force reinstall plugin anyway? [N/y]"
        if ($fp -notmatch '^[Yy]') { $skipPlugin = $true }
    }

    if (-not $skipPlugin) {
        Write-Step ("Downloading plugin {0} ({1})" -f $plugin.Key, $asset.name)
        $pluginPkg = Join-Path $cacheDir $asset.name
        $dlUrl = $asset.browser_download_url -replace '^https?://', ($GhMirror + '/')
        # Invoke-DownloadFile reuses a complete cached archive or re-downloads
        # (and deletes) a partial/locked one automatically.
        Invoke-DownloadFile $dlUrl $pluginPkg ("Downloading plugin {0}" -f $plugin.Key)

        Write-Step "Extracting and installing plugin"
        $pluginExtract = Join-Path $cacheDir 'plugin_extract'
        New-Item -ItemType Directory -Path $pluginExtract -Force | Out-Null
        if ($asset.name -like '*.zip') {
            Expand-Archive -Path $pluginPkg -DestinationPath $pluginExtract -Force
        } else {
            & $sevenZip x $pluginPkg -o"$pluginExtract" -y | Out-Null
        }
        $verDlls = Get-ChildItem $pluginExtract -Recurse -Filter version.dll
        # Prefer the build matching the detected architecture (chrome_green: x64\, chrome_plus: x64\App\).
        $verDll = $verDlls | Where-Object { $_.FullName -match ('\\' + $sysArch + '\\') } | Select-Object -First 1
        if (-not $verDll) { $verDll = $verDlls | Select-Object -First 1 }
        if (-not $verDll) { throw "No version.dll found in the plugin archive; portability cannot be applied." }
        $pluginRoot = $verDll.DirectoryName
        # Ensure Chrome is not running (files must be writable for plugin copy).
        Ensure-NoChromeProcess $appDir | Out-Null
        # For chrome_plus, preserve an existing chrome++.ini (user config).
        if ($plugin.Key -eq 'chrome_plus') {
            $iniFile = Join-Path $appDir 'chrome++.ini'
            Get-ChildItem (Join-Path $pluginRoot '*') | ForEach-Object {
                if ($_.Name -eq 'chrome++.ini' -and (Test-Path $iniFile)) { return }
                Copy-Item $_.FullName $appDir -Force
            }
        } else {
            Copy-Item (Join-Path $pluginRoot '*') $appDir -Recurse -Force
        }
        Write-Status ("Plugin installed: {0}" -f $plugin.Key)

        # Free the extracted files; keep the cached archive for next time.
        Clear-ExtractDirs
    } else {
        Write-Step "Plugin skipped (already up to date: $tag)"
    }
} catch {
    # A plugin failure must not discard an already-installed Chrome.
    Write-Host ("  [warning] Plugin install failed: {0}" -f $_.Exception.Message) -ForegroundColor Yellow
    Write-Host "  Chrome is installed; place version.dll into the App directory manually later." -ForegroundColor Yellow
}

# --- Save configuration (persisted across updates) ---
$cfg = [ordered]@{
    channel = $channelKey
    download = $dlHost
    plugin = $plugin.Key
    installDir = $InstallDir
    version = $chromeVer
    pluginVersion = $pluginVersion
    updated = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
}
Save-Config $configFile $cfg

# --- Cleanup menu ---
# Offer to clean cached artifacts so the temp directory stays tidy.
#   1) Clean all      - delete the entire cache directory (no trace left)
#   2) Keep only config - delete installers/7za/plugin archives, keep config  (default)
#   3) No cleanup      - keep everything as-is for the fastest next run
Write-Host ""
$cleanupIdx = Read-Menu 'Clean up cached files' @(
    'Clean all (delete installers, 7za, plugins)',
    'Keep only config (delete everything else)',
    'No cleanup (keep all cached files)'
) 2
if ($null -eq $cleanupIdx) { $cleanupIdx = 2 }
if ($cleanupIdx -eq 1) {
    # Clean everything including config (will be recreated next run).
    try { Remove-Item -Path $cacheDir -Recurse -Force -ErrorAction SilentlyContinue } catch { }
} elseif ($cleanupIdx -eq 2) {
    # Keep only config; remove everything else.
    Clear-ExtractDirs
    Get-ChildItem $cacheDir -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'chrome_green_install_config.json' } | ForEach-Object {
        if ($_.PSIsContainer) {
            try { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue } catch { }
        } else {
            try { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue } catch { }
        }
    }
} else {
    # No cleanup: leave the cache directory untouched.
}

# --- Cleanup result ---
Write-Host ""
Write-Host "Cleanup result:" -ForegroundColor Cyan
if ($cleanupIdx -eq 1) {
    if (Test-Path $cacheDir) {
        Write-Status ("  Cache directory NOT fully removed: $cacheDir")
    } else {
        Write-Status ("  Removed entire cache directory (no trace left): $cacheDir")
    }
} elseif ($cleanupIdx -eq 2) {
    $kept = Join-Path $cacheDir 'chrome_green_install_config.json'
    if (Test-Path $kept) {
        Write-Status ("  Removed installers, 7za and plugin archives.")
        Write-Status ("  Kept config: $kept")
    } else {
        Write-Status ("  Cleaned; config was absent.")
    }
} else {
    Write-Status ("  No cleanup performed; all cached files kept: $cacheDir")
}

# --- Done ---
Write-Banner "Installation complete"
Write-Host "  Mode        : $(if ($isFirst) { 'First install' } elseif ($skipChrome -and $skipPlugin) { 'No changes (up to date)' } else { 'Update' })" -ForegroundColor White
Write-Host "  Chrome ver   : $chromeVer" -ForegroundColor White
Write-Host ("  Channel     : {0}" -f $channelKey) -ForegroundColor White
Write-Host ("  Download    : {0}" -f $dlHost) -ForegroundColor White
Write-Host ("  Plugin      : {0}  (version.dll in place)" -f $plugin.Key) -ForegroundColor White
Write-Host ("  Install dir : {0}" -f $InstallDir) -ForegroundColor White
Write-Host ""
Write-Host "  Launch: $appDir\chrome.exe" -ForegroundColor Green
Write-Host "  User data can be kept portable by running with --user-data-dir=`"$dataDir`"" -ForegroundColor Green
Write-Host "  Future updates: run this script again; the saved config will be offered." -ForegroundColor Green
Write-Host ""

# Open the install directory
try { Start-Process 'explorer.exe' -ArgumentList $InstallDir } catch { }
















