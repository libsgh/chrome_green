#include "updater.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include "config.h"
#include "diaglog.h"
#include "downloader.h"
#include "httpserver.h"
#include "appid.h"
#include "update.h"
#include "utils.h"
#include "7z_extract.h"
#include "detours.h"
#include <wchar.h>
#include <tlhelp32.h>

namespace {

std::atomic<bool> check_in_progress_{false};
std::thread check_thread_;

// Scan the updates/ directory for Chrome update package files.
// Returns the full path of the first .7z or .exe found, or empty string if none.
// Chrome installers from Google are .exe files (self-extracting 7z archives),
// so we search for both extensions. Skips:
//   - chrome_green* (self-update DLL)
//   - chrome.exe, chrome_proxy.exe (the browser itself, not an installer)
// This enables offline installation: user places a .7z or .exe in updates/
// folder and TriggerDownload will use it instead of downloading from internet.
std::wstring FindLocalPackage() {
  std::wstring updates_dir = GetSelfDllDir() + L"\\updates";

  // Search for both .7z and .exe files
  const wchar_t* patterns[] = {L"*.7z", L"*.exe"};
  for (int p = 0; p < 2; p++) {
    std::wstring search = updates_dir + L"\\" + patterns[p];

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) continue;

    std::wstring result;
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring name = fd.cFileName;
        // Skip non-installer files
        if (_wcsicmp(name.c_str(), L"chrome.exe") == 0) continue;
        if (_wcsicmp(name.c_str(), L"chrome_proxy.exe") == 0) continue;
        if (name.find(L"chrome_green") != std::wstring::npos) continue;
        result = updates_dir + L"\\" + name;
        break;
      }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    if (!result.empty()) return result;
  }

  return L"";
}

} // namespace


#ifndef IDR_UPDATER_EXE
#define IDR_UPDATER_EXE 1001
#endif

// Launch the standalone GUI update window. Extracts the embedded updater exe
// to <install>\updates\, writes a manifest (UTF-16LE) carrying all decisions,
// then launches it detached. Returns false on failure.
bool LaunchUpdateWindow() {
  auto state = GetUpdateStateSnapshot();
  DWORD pid = GetCurrentProcessId();

  wchar_t chrome_exe[MAX_PATH];
  GetModuleFileNameW(nullptr, chrome_exe, MAX_PATH);

  std::wstring app_dir = GetAppDir();
  std::wstring self_dll_dir = GetSelfDllDir();
  std::wstring updates_dir = self_dll_dir + L"\\updates";
  CreateDirectoryW(updates_dir.c_str(), nullptr);
  std::wstring exe_path = updates_dir + L"\\chrome_green_updater.exe";
  std::wstring manifest_path = updates_dir + L"\\chrome_green_updater_manifest.txt";

  // 1. Extract the embedded updater exe from our own resources.
  // NOTE: use hInstance (this DLL's module), NOT GetModuleHandleW(nullptr)
  // which would return chrome.exe — the RCDATA lives inside version.dll.
  HMODULE hMod = hInstance;
  // RCDATA is registered as the predefined numeric type RT_RCDATA(10); passing
  // the string L"RCDATA" would NOT match it.
  HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCE(IDR_UPDATER_EXE), RT_RCDATA);
  if (!hRes) {
    AddDebugLog("LaunchUpdateWindow: FindResource(RCDATA 1001) failed");
    return false;
  }
  HGLOBAL hGlob = LoadResource(hMod, hRes);
  if (!hGlob) {
    AddDebugLog("LaunchUpdateWindow: LoadResource failed");
    return false;
  }
  DWORD size = SizeofResource(hMod, hRes);
  LPVOID data = LockResource(hGlob);
  if (!data || size == 0) {
    AddDebugLog("LaunchUpdateWindow: LockResource/SizeofResource failed");
    return false;
  }
  {
    HANDLE hF = CreateFileW(exe_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hF == INVALID_HANDLE_VALUE) {
      const DWORD err = ::GetLastError();
      AddDebugLog("LaunchUpdateWindow: cannot create updater exe: " +
                  std::string(exe_path.begin(), exe_path.end()));
      return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(hF, data, size, &written, nullptr);
    CloseHandle(hF);
    if (!ok || written != size) {
      AddDebugLog("LaunchUpdateWindow: write updater exe incomplete");
      DeleteFileW(exe_path.c_str());
      return false;
    }
  }

  // 2. Write the manifest (UTF-16LE with BOM) consumed by the updater exe.
  {
    std::wstring new_dll;
    if (state.self_update_ready && !state.self_download_path.empty()) {
      new_dll = Utf8ToWstr(state.self_download_path);
      if (new_dll.find(L':') == std::wstring::npos) {
        new_dll = self_dll_dir + L"\\" + new_dll;
      }
    }
    std::wstring keep_version = Utf8ToWstr(
        state.latest_version.empty() ? state.current_version : state.latest_version);

    std::wstring m;
    m += L"APP_DIR=" + app_dir + L"\r\n";
    m += L"SELF_DLL_DIR=" + self_dll_dir + L"\r\n";
    m += L"PID=" + std::to_wstring(pid) + L"\r\n";
    m += L"CHROME_EXE=" + std::wstring(chrome_exe) + L"\r\n";
    m += L"KEEP_OLD=" + std::wstring(config.KeepOldVersions() ? L"1" : L"0") + L"\r\n";
    m += L"KEEP_INSTALLER=" + std::wstring(config.KeepInstaller() ? L"1" : L"0") + L"\r\n";
    m += L"KEEP_VERSION=" + keep_version + L"\r\n";
    m += L"SELF_UPDATE_READY=" + std::wstring(state.self_update_ready ? L"1" : L"0") + L"\r\n";
    m += L"SELF_DLL_NEW=" + new_dll + L"\r\n";
    m += L"STATE_FILE=" + GetUpdateStatePath() + L"\r\n";
    // Pass the config page's appearance theme so the update window matches it.
    // Value is "auto"/"light"/"dark" (same as settings.theme). The exe resolves
    // "auto" against the OS using the identical key Chrome reads for
    // prefers-color-scheme, keeping the window in sync with the config page.
    m += L"THEME=" + Utf8ToWstr(config.GetTheme()) + L"\r\n";

    HANDLE hF = CreateFileW(manifest_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hF == INVALID_HANDLE_VALUE) {
      const DWORD err = ::GetLastError();
      AddDebugLog("LaunchUpdateWindow: cannot create manifest");
      DeleteFileW(exe_path.c_str());
      return false;
    }
    WORD bom = 0xFEFF;
    DWORD w = 0;
    WriteFile(hF, &bom, 2, &w, nullptr);
    WriteFile(hF, m.c_str(), (DWORD)(m.size() * 2), &w, nullptr);
    CloseHandle(hF);
  }

  // 3. Launch the updater exe detached (survives Chrome exiting), visible window.
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_SHOWNORMAL;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd_line = L"\"" + exe_path + L"\"";
  DWORD flags = CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
  BOOL ok = CreateProcessW(nullptr, cmd_line.data(), nullptr, nullptr, FALSE,
                           flags, nullptr, nullptr, &si, &pi);
  if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
    // Job does not allow breakaway — retry without it (still detached from
    // console / ctrl-c group, just may remain inside the job).
    flags = CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP;
    ok = CreateProcessW(nullptr, cmd_line.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, nullptr, &si, &pi);
  }
  if (!ok) {
    DWORD err = GetLastError();
    AddDebugLog("LaunchUpdateWindow: CreateProcessW failed: " + std::to_string(err));
    DeleteFileW(exe_path.c_str());
    DeleteFileW(manifest_path.c_str());
    return false;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  AddDebugLog("Update window launched successfully");
  return true;
}

namespace { // reopen anonymous namespace for internal helpers

// Recursively move all files from src_dir to dst_dir, overwriting existing files.
// Subdirectories are merged (created if they don't exist, contents moved recursively).
// For locked files (e.g. running chrome.exe), uses rename-to-.old trick:
// NTFS allows renaming a file in use (same volume), so we rename the old file,
// move the new one into place, and the old file becomes .old for cleanup later.
void MoveAllFiles(const std::wstring& src_dir, const std::wstring& dst_dir) {
  // Ensure destination directory exists
  CreateDirectoryW(dst_dir.c_str(), nullptr);

  std::wstring search = src_dir + L"\\*";
  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) return;

  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;

    std::wstring src = src_dir + L"\\" + name;
    std::wstring dst = dst_dir + L"\\" + name;

    // Skip the running process's own executable. Replacing chrome.exe
    // while the current process is running causes version mismatch when
    // Chrome spawns child processes: parent is old version, child loads
    // the new exe → IPC breaks → Chrome crashes without showing a window.
    //
    // Workaround: copy new chrome.exe to chrome.exe.new. On the NEXT Chrome
    // startup, the new Chrome process (running the OLD exe but new DLLs)
    // will detect chrome.exe.new and finish the swap. We also delete
    // chrome.exe.old if it exists (leftover from a previous MoveAllFiles
    // attempt that was then superseded by xcopy).
    if (_wcsicmp(name.c_str(), L"chrome.exe") == 0) {
      wchar_t self_exe[MAX_PATH];
      if (GetModuleFileNameW(nullptr, self_exe, MAX_PATH) &&
          _wcsicmp(dst.c_str(), self_exe) == 0) {
        std::wstring new_placeholder = dst_dir + L"\\chrome.exe.new";
        if (CopyFileW(src.c_str(), new_placeholder.c_str(), FALSE)) {
          AddDebugLog("MoveAllFiles: stashed new chrome.exe as chrome.exe.new");
        } else {
          AddDebugLog("MoveAllFiles: failed to copy chrome.exe.new, error=" +
                      std::to_string(GetLastError()));
        }
        // Also remove any stale .old from previous attempt
        std::wstring old_path = dst + L".old";
        DeleteFileW(old_path.c_str());
        continue;
      }
    }

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      // Recursively move subdirectory
      MoveAllFiles(src, dst);
      RemoveDirectoryW(src.c_str());
    } else {
      if (PathFileExistsW(dst.c_str())) {
        // Try to delete the destination file first
        if (!DeleteFileW(dst.c_str())) {
          // File is likely locked (e.g. running chrome.exe).
          // On NTFS, we can RENAME a locked file (same volume), then move
          // the new one into place. The running process continues using
          // the renamed file via its existing file mapping.
          std::wstring dst_old = dst + L".old";
          // Remove any previous .old file first
          DeleteFileW(dst_old.c_str());
          if (MoveFileW(dst.c_str(), dst_old.c_str())) {
            AddDebugLog("Renamed locked file: " +
                        std::string(dst.begin(), dst.end()) + " -> .old");
            // CRITICAL: If we rename dst → .old, we MUST succeed in moving
            // src → dst. If src → dst fails, we restore .old → dst to prevent
            // the file from being permanently lost (which would cause Chrome's
            // 0x7E "Failed to load Chrome DLL" crash on startup).
            if (!MoveFileW(src.c_str(), dst.c_str())) {
              AddDebugLog("Failed to move: " + std::string(src.begin(), src.end()) +
                          " -> " + std::string(dst.begin(), dst.end()) +
                          ", error=" + std::to_string(GetLastError()));
              // Restore: rename .old back to original name so the file isn't lost
              MoveFileW(dst_old.c_str(), dst.c_str());
              AddDebugLog("Restored .old -> " + std::string(dst.begin(), dst.end()));
              continue;  // Skip this file — can't replace it right now
            }
          } else {
            AddDebugLog("Failed to rename locked file: " +
                        std::string(dst.begin(), dst.end()) +
                        ", error=" + std::to_string(GetLastError()));
            // Skip this file — can't replace it right now
            continue;
          }
        } else {
          // Delete succeeded — dst slot is empty, try moving src in
          if (!MoveFileW(src.c_str(), dst.c_str())) {
            AddDebugLog("Failed to move: " + std::string(src.begin(), src.end()) +
                        " -> " + std::string(dst.begin(), dst.end()) +
                        ", error=" + std::to_string(GetLastError()));
            continue;  // Skip this file
          }
        }
      } else {
        // No existing dst file — just move src in directly
        if (!MoveFileW(src.c_str(), dst.c_str())) {
          AddDebugLog("Failed to move: " + std::string(src.begin(), src.end()) +
                      " -> " + std::string(dst.begin(), dst.end()) +
                      ", error=" + std::to_string(GetLastError()));
          continue;  // Skip this file
        }
      }
    }
  } while (FindNextFileW(hFind, &fd));

  FindClose(hFind);
}

// Recursively delete a directory and all its contents.
// For locked files that can't be deleted, rename them to .old (NTFS allows
// renaming locked files on the same volume). These .old files will be cleaned
// up on the next Chrome startup.
void RemoveDirectoryRecursive(const std::wstring& dir) {
  std::wstring search = dir + L"\\*";
  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) {
    RemoveDirectoryW(dir.c_str());
    return;
  }

  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring path = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      RemoveDirectoryRecursive(path);
    } else {
      if (!DeleteFileW(path.c_str())) {
        // File is likely locked (e.g. by a still-running child process or
        // antivirus). Try renaming to .old — NTFS allows renaming locked
        // files on the same volume. The .old file will be cleaned up on the
        // next Chrome startup.
        std::wstring old_path = path + L".old";
        DeleteFileW(old_path.c_str());  // Remove any previous .old first
        if (MoveFileW(path.c_str(), old_path.c_str())) {
          AddDebugLog("Renamed locked file: " +
                      std::string(path.begin(), path.end()) + " -> .old");
        } else {
          AddDebugLog("Failed to delete or rename: " +
                      std::string(path.begin(), path.end()) +
                      ", error=" + std::to_string(GetLastError()));
        }
      }
    }
  } while (FindNextFileW(hFind, &fd));

  FindClose(hFind);
  RemoveDirectoryW(dir.c_str());
}

// Recursively clean up .old files in a directory tree.
// .old files are created when a locked file couldn't be deleted during an
// update — they were renamed instead, and should be cleaned on the next startup.
// SAFETY: Only deletes .old files if the corresponding real file exists in the
// same directory. This prevents deleting a .old file that is actually the only
// remaining copy of a critical file (e.g. chrome.dll.old when chrome.dll is
// missing — which would cause Chrome's 0x7E "Failed to load Chrome DLL" crash).
// NOTE: This function ONLY deletes .old files. It does NOT remove directories.
// Directory removal is handled by CleanupOldVersionDirs which has version-checking
// logic to prevent deleting the current version's directory.
void CleanupDotOldFiles(const std::wstring& dir) {
  std::wstring search = dir + L"\\*";
  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) return;

  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring path = dir + L"\\" + name;

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      // Recurse into subdirectories (e.g. version dirs may contain .old files)
      CleanupDotOldFiles(path);
    } else {
      // Delete .old files (left by RemoveDirectoryRecursive or MoveAllFiles)
      // SAFETY: only delete .old if the real file exists — otherwise the .old
      // file might be the ONLY copy (e.g. chrome.dll was renamed to .old but
      // the new file wasn't moved in). Deleting it would make chrome.dll
      // permanently missing, causing Chrome's 0x7E crash.
      if (name.size() > 4 && name.substr(name.size() - 4) == L".old") {
        std::wstring real_name = name.substr(0, name.size() - 4);
        std::wstring real_path = dir + L"\\" + real_name;
        if (PathFileExistsW(real_path.c_str())) {
          DeleteFileW(path.c_str());
        }
      }
    }
  } while (FindNextFileW(hFind, &fd));

  FindClose(hFind);
}

// Delete old Chrome version directories in app_dir that are not equal to
// keep_version. Looks for directories matching "<major>.<minor>.<build>.<patch>".
// SAFETY: verifies that the keep_version directory exists and contains chrome.dll
// before deleting any version directory. If keep_version dir is missing or
// doesn't have chrome.dll, NO directories are deleted — something is wrong and
// we shouldn't risk deleting the current version.
void CleanupOldVersionDirs(const std::wstring& app_dir,
                           const std::string& keep_version) {
  if (keep_version.empty()) {
    AddDebugLog("CleanupOldVersionDirs: keep_version is empty, skipping cleanup");
    return;
  }

  std::wstring keep = Utf8ToWstr(keep_version);

  // Safety check: verify keep_version directory exists and has chrome.dll
  std::wstring keep_dir = app_dir + L"\\" + keep;
  std::wstring keep_chrome_dll = keep_dir + L"\\chrome.dll";
  if (!PathFileExistsW(keep_chrome_dll.c_str())) {
    AddDebugLog("CleanupOldVersionDirs: keep_version dir missing chrome.dll: " +
                std::string(keep.begin(), keep.end()) + ", skipping cleanup");
    return;
  }

  std::wstring search = app_dir + L"\\*";
  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) return;

  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    // Simple version pattern: digits.digits.digits.digits
    bool looks_like_version = true;
    int dot_count = 0;
    for (wchar_t c : name) {
      if (c == L'.') {
        dot_count++;
      } else if (c < L'0' || c > L'9') {
        looks_like_version = false;
        break;
      }
    }
    if (looks_like_version && dot_count == 3 && name != keep) {
      std::wstring old_dir = app_dir + L"\\" + name;
      RemoveDirectoryRecursive(old_dir);
      // Retry RemoveDirectoryW — sometimes the first call fails because
      // a file handle was just released (race with AV or Chrome child process).
      RemoveDirectoryW(old_dir.c_str());
      if (PathFileExistsW(old_dir.c_str())) {
        AddDebugLog("WARNING: old version dir still exists after cleanup: " +
                    std::string(name.begin(), name.end()));
      } else {
        AddDebugLog("Removed old Chrome version dir: " +
                    std::string(name.begin(), name.end()));
      }
    }
  } while (FindNextFileW(hFind, &fd));

  FindClose(hFind);
}

// Background thread: check for updates via Omaha protocol.
void CheckThread() {
  check_in_progress_.store(true);
  SetUpdateState(UpdateState::kChecking);

  // Read channel/arch/proxy from live state (may have been changed by API)
  UpdateChannel active_channel;
  UpdateArch active_arch;
  std::string active_proxy;
  std::string active_proxy_type;
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    active_channel = g_update_state.channel;
    active_arch = g_update_state.arch;
    active_proxy = g_update_state.proxy;
    active_proxy_type = g_update_state.proxy_type;
  }

  UpdateInfo info = CheckForUpdates(active_channel, active_arch,
                                     active_proxy, active_proxy_type);

  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.last_check_time = static_cast<int64_t>(time(nullptr));

    if (info.has_update) {
      g_update_state.latest_version = info.version;
      g_update_state.sha256 = info.sha256;
      g_update_state.download_size = info.size;

      // Compare versions
      if (info.version != g_update_state.current_version) {
        g_update_state.state = UpdateState::kAvailable;
      } else {
        g_update_state.state = UpdateState::kIdle;
      }
    } else {
      g_update_state.state = UpdateState::kIdle;
    }
  }
  SaveUpdateState();

  // Auto-download if enabled and update is available
  if (info.has_update && config.IsAutoDownload() &&
      info.version != g_update_state.current_version) {
    TriggerDownload();
  }

  check_in_progress_.store(false);
}

}  // namespace

bool HasLocalPackage() {
  return !FindLocalPackage().empty();
}

void CleanUpdateWorkDirs() {
  // A new download may have been started between the reset and now (e.g. the
  // user immediately re-checked with auto-download enabled). Never touch the
  // working folders while a download is writing into them.
  if (Downloader::Instance().IsDownloading()) {
    AddDebugLog("CleanUpdateWorkDirs: a download is in progress, skipping");
    return;
  }

  std::wstring dll_dir = GetSelfDllDir();

  // update_temp/ is pure extraction scratch — always wipe it.
  std::wstring temp_dir = dll_dir + L"\\update_temp";
  if (PathFileExistsW(temp_dir.c_str())) {
    RemoveDirectoryRecursive(temp_dir);
    RemoveDirectoryW(temp_dir.c_str());
  }

  std::wstring updates_dir = dll_dir + L"\\updates";
  if (!PathFileExistsW(updates_dir.c_str())) return;

  // Preserve the self_update staging dir when a self-update is downloaded
  // and waiting to be applied on restart — wiping it would break the
  // pending version.dll swap.
  bool keep_self_update = false;
  {
    auto st = GetUpdateStateSnapshot();
    keep_self_update = st.self_update_ready;
  }

  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW((updates_dir + L"\\*").c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) return;

  std::vector<std::wstring> locked;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    if (keep_self_update && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        _wcsicmp(name.c_str(), L"self_update") == 0) {
      continue;
    }
    std::wstring path = updates_dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      RemoveDirectoryRecursive(path);
      if (PathFileExistsW(path.c_str())) locked.push_back(path);
    } else if (!DeleteFileW(path.c_str())) {
      locked.push_back(path);
    }
  } while (FindNextFileW(hFind, &fd));
  FindClose(hFind);

  // Locked leftovers: retry for a few seconds (handles are often released a
  // moment later by the dying download thread or antivirus), then rename to
  // .old — NTFS allows renaming an open file on the same volume. Startup
  // cleanup deletes the .old files on the next launch.
  for (const auto& path : locked) {
    bool gone = false;
    for (int i = 0; i < 10 && !gone; i++) {
      Sleep(500);
      if (!PathFileExistsW(path.c_str())) break;
      if (DeleteFileW(path.c_str()) || RemoveDirectoryW(path.c_str())) {
        gone = !PathFileExistsW(path.c_str());
      }
    }
    if (!gone && PathFileExistsW(path.c_str())) {
      DWORD attr = GetFileAttributesW(path.c_str());
      if (attr != INVALID_FILE_ATTRIBUTES &&
          !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring old_path = path + L".old";
        DeleteFileW(old_path.c_str());  // remove any previous .old first
        if (MoveFileW(path.c_str(), old_path.c_str())) {
          AddDebugLog("CleanUpdateWorkDirs: renamed locked file to .old: " +
                      WstrToUtf8(path));
        } else {
          AddDebugLog("CleanUpdateWorkDirs: cannot delete or rename: " +
                      WstrToUtf8(path) + " (error=" +
                      std::to_string(GetLastError()) + ")");
        }
      }
    }
  }

  // Remove the now-empty updates/ folder itself (no-op if anything is left).
  RemoveDirectoryW(updates_dir.c_str());
  AddDebugLog("CleanUpdateWorkDirs: update working folders cleaned");
}

void TriggerUpdateCheck() {
  if (check_in_progress_.load()) return;

  if (check_thread_.joinable()) {
    check_thread_.join();
  }

  check_thread_ = std::thread(CheckThread);
}

void TriggerDownload() {
  auto state = GetUpdateStateSnapshot();

  // A download thread is already running (e.g. a just-cancelled one still
  // winding down after a network failure, or one blocked in a WinHTTP read
  // until its timeout). Downloader::Start would silently refuse it AFTER
  // TriggerDownload flipped the state to kDownloading below — leaving a
  // progress bar with no download behind. Bail out early instead.
  if (Downloader::Instance().IsDownloading()) {
    AddDebugLog("TriggerDownload: a download is already in progress, ignoring");
    return;
  }

  // Offline install: check for a local .7z package in the updates folder.
  // If found, skip the entire download flow and use the local file directly.
  // This enables:
  //   1. Faster testing (no need to wait for slow downloads)
  //   2. Offline installation (user manually places .7z in updates/ folder)
  std::wstring local_package = FindLocalPackage();
  if (!local_package.empty()) {
    AddDebugLog("Found local package, skipping download: " +
                WstrToUtf8(local_package));

    // Get file size for status display
    int64_t file_size = 0;
    HANDLE hFile = CreateFileW(local_package.c_str(), GENERIC_READ,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER li;
      if (GetFileSizeEx(hFile, &li)) file_size = li.QuadPart;
      CloseHandle(hFile);
    }

    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.download_path = WstrToUtf8(local_package);
      g_update_state.download_size = file_size;
      g_update_state.download_progress = 100;
      g_update_state.downloaded_bytes = file_size;
      // Skip SHA-256 verification — offline file, user trusts it
      g_update_state.sha256.clear();
      g_update_state.state = UpdateState::kReady;
    }
    SaveUpdateState();

    // Extract immediately (same as normal download completion)
    ApplyPendingUpdate();
    return;
  }

  // Normal download flow — requires kAvailable or kError state
  if (state.state != UpdateState::kAvailable &&
      state.state != UpdateState::kError) {
    return;
  }

  // Re-check to get fresh download URLs
  if (state.latest_version.empty()) return;

  // Flip to "downloading" immediately so the config page shows the progress
  // bar + cancel button without waiting for the (network) Omaha re-check and
  // WinHTTP connect below. Progress stays at 0 until bytes actually arrive.
  // If the re-check or download later fails, SetUpdateError() overrides this
  // with kError, so there is no stuck "downloading" state.
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.state = UpdateState::kDownloading;
    g_update_state.download_progress = 0;
    g_update_state.downloaded_bytes = 0;
    g_update_state.download_speed = 0;
    g_update_state.download_eta = 0;
    g_update_state.error_message.clear();
  }
  SaveUpdateState();

  // Re-query Omaha to get download URLs (they may have expired)
  UpdateInfo info = CheckForUpdates(state.channel, state.arch,
                                     state.proxy, state.proxy_type);
  if (!info.has_update || info.urls.empty()) {
    SetUpdateError("No download URL available");
    return;
  }

  // Build download path
  std::wstring data_dir = GetSelfDllDir() + L"\\updates";
  CreateDirectoryW(data_dir.c_str(), nullptr);

  // Pick URL by download_source preference
  std::string url;
  const char* domains[] = {"edgedl.me.gvt1.com", "dl.google.com", "www.google.com", "redirector.gvt1.com"};
  int src = (state.download_source >= 0 && state.download_source < 4) ? state.download_source : 1;
  for (const auto& u : info.urls) {
    if (u.find(domains[src]) != std::string::npos) { url = u; break; }
  }
  if (url.empty()) {
    AddDebugLog("Download source '" + std::string(domains[src]) + "' not in Omaha URL list, using first");
    url = info.urls[0];
  }

  // Extract filename from URL
  auto last_slash = url.find_last_of('/');
  std::string filename = (last_slash != std::string::npos)
                             ? url.substr(last_slash + 1) : "chrome_update.7z";
  std::wstring filename_w(filename.begin(), filename.end());
  std::wstring download_path = data_dir + L"\\" + filename_w;

  // Store download info in state
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.latest_version = info.version;
    g_update_state.sha256 = info.sha256;
    g_update_state.download_size = info.size;
    g_update_state.download_path = WstrToUtf8(download_path);
  }
  SaveUpdateState();

  AddDebugLog("Starting download from " + std::string(domains[src]) + ": " + url);
  Downloader::Instance().Start(url, download_path, info.sha256);
}

bool ApplyPendingUpdate() {
  auto state = GetUpdateStateSnapshot();

  AddDebugLog("ApplyPendingUpdate: state=" + std::to_string(static_cast<int>(state.state)) +
              ", download_path=" + state.download_path);
  // Self-update is applied by the standalone GUI update window, which the
  // caller launches after this returns. The window waits for Chrome to exit,
  // then replaces version.dll. We don't do MoveFileExW here because that
  // requires an OS reboot. If self_update_ready is set, just proceed — the
  // caller will launch the update window and exit Chrome.

  if (state.state != UpdateState::kReady) {
    return false;
  }

  if (state.download_path.empty()) {
    return false;
  }

  SetUpdateState(UpdateState::kApplying);

  std::wstring archive = Utf8ToWstr(state.download_path);

  if (!PathFileExistsW(archive.c_str())) {
    SetUpdateError("Downloaded file not found");
    return false;
  }

  // Create temp extraction directory
  std::wstring temp_dir = GetSelfDllDir() + L"\\update_temp";
  RemoveDirectoryW(temp_dir.c_str());  // Clean any previous extraction
  CreateDirectoryW(temp_dir.c_str(), nullptr);

  // First extraction: outer .7z → chrome.7z + Setup.exe etc.
  if (!Extract7zLegacy(archive, temp_dir)) {
    // Clean up any partial extraction
    RemoveDirectoryRecursive(temp_dir);
    RemoveDirectoryW(temp_dir.c_str());
    SetUpdateError("Failed to extract archive. The file may not be a valid Chrome "
                   "update package. Expected: xxx_chrome_installer_uncompressed.exe "
                   "(7z SFX from Google's Omaha update servers). Chrome web installer "
                   "(ChromeSetup.exe) and standalone installer are NOT supported.");
    return false;
  }

  // Check for inner chrome.7z
  std::wstring inner_7z = temp_dir + L"\\chrome.7z";
  if (PathFileExistsW(inner_7z.c_str())) {
    // Second extraction: chrome.7z → Chrome-bin/ + other files
    if (!Extract7zLegacy(inner_7z, temp_dir)) {
      RemoveDirectoryRecursive(temp_dir);
      RemoveDirectoryW(temp_dir.c_str());
      SetUpdateError("Failed to extract inner chrome.7z");
      return false;
    }
  }

  // Optionally remove installer file if keep_installer is false
  if (!config.KeepInstaller()) {
    std::wstring archive_w = Utf8ToWstr(state.download_path);
    if (PathFileExistsW(archive_w.c_str())) {
      DeleteFileW(archive_w.c_str());
      AddDebugLog("Removed installer file: " + state.download_path);
    }
  }

  // Mark state as pending — files will be moved on next Chrome launch
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.state = UpdateState::kPendingApply;
    g_update_state.download_path.clear();  // no longer needed
    g_update_state.download_progress = 0;
    g_update_state.downloaded_bytes = 0;
  }
  SaveUpdateState();

  DebugLog(L"Update extracted to temp dir, pending apply on next launch");
  return true;
}

void InitUpdater() {
  // Load saved state
  LoadUpdateState();

  // --- Auto-apply a staged ChromeGreen (self) update on startup ---
  // The self-update replaces version.dll, which is locked while this process
  // is alive, so it can only be applied AFTER Chrome exits. The standalone
  // updater window waits for this Chrome process to exit, copies the staged
  // version.dll over the live one, then relaunches Chrome. This makes a
  // pending self-update apply automatically even if the user closed Chrome
  // and restarted it manually instead of clicking "重启已完成更新".
  {
    auto st = GetUpdateStateSnapshot();
    if (st.self_update_ready && !st.self_download_path.empty()) {
      std::wstring staged = Utf8ToWstr(st.self_download_path);
      if (staged.find(L':') == std::wstring::npos)
        staged = GetSelfDllDir() + L"\\" + staged;
      if (PathFileExistsW(staged.c_str())) {
        AddDebugLog("InitUpdater: staged self-update found, launching updater to apply");
        // Also apply a pending Chrome update (combined restart), matching the
        // /api/self-update/apply endpoint. Harmless if none is pending.
        ApplyPendingUpdate();
        if (LaunchUpdateWindow()) {
          // Clear self-update state so the relaunched Chrome does not loop.
          {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            g_update_state.self_update_ready = false;
            g_update_state.self_self_download_progress = 0;
            g_update_state.self_download_path.clear();
            g_update_state.self_has_update = false;
            g_update_state.self_latest_version = "";
          }
          SaveUpdateState();
          // Exit so the updater can replace version.dll and relaunch Chrome.
          ExitProcess(0);
        } else {
          AddDebugLog("InitUpdater: LaunchUpdateWindow failed at startup");
        }
      } else {
        // Staged dll missing — clear to avoid repeated failed attempts.
        AddDebugLog("InitUpdater: self_update_ready but staged dll missing, clearing");
        {
          std::lock_guard<std::mutex> lock(g_update_mutex);
          g_update_state.self_update_ready = false;
          g_update_state.self_download_path.clear();
        }
        SaveUpdateState();
      }
    }
  }

  // Get current Chrome version from chrome.exe file version info ASAP.
  // This is the authoritative version — used by CleanupOldVersionDirs to
  // determine which directory to keep. Must run BEFORE any cleanup.
  std::string chrome_ver = GetInstalledChromeVersion();
  if (!chrome_ver.empty()) {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.current_version = chrome_ver;
  }

  // Recover from states that cannot survive a restart: the threads driving
  // "checking"/"downloading"/"applying" died with the previous process, so a
  // persisted state like that leaves the config page showing a frozen
  // progress bar forever with no way out. Fall back to "available" when the
  // interrupted update is still newer than the installed version, else idle.
  {
    const char* stale = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      if (g_update_state.state == UpdateState::kChecking) stale = "checking";
      else if (g_update_state.state == UpdateState::kDownloading) stale = "downloading";
      else if (g_update_state.state == UpdateState::kApplying) stale = "applying";
      if (stale) {
        if (!g_update_state.latest_version.empty() &&
            g_update_state.latest_version != g_update_state.current_version) {
          g_update_state.state = UpdateState::kAvailable;
        } else {
          g_update_state.state = UpdateState::kIdle;
        }
        g_update_state.download_progress = 0;
        g_update_state.downloaded_bytes = 0;
        g_update_state.download_speed = 0;
        g_update_state.download_eta = 0;
        g_update_state.download_path.clear();
        g_update_state.error_message.clear();
      }
    }
    if (stale) {
      AddDebugLog(std::string("InitUpdater: stale '") + stale +
                  "' state at startup (its thread died with the previous "
                  "process), resetting");
      SaveUpdateState();
    }
  }

  // EARLY diagnostic (ring buffer only): snapshot state so we can see what
  // happened on a FAILED startup (the ring buffer is lost when Chrome crashes
  // with 0x7E). Persisted file logging (chrome_green_diag.log) removed.
  {
    auto snap = GetUpdateStateSnapshot();
    AddDebugLog("InitUpdater: state=" + std::to_string((int)snap.state) +
                " current=" + snap.current_version + " latest=" + snap.latest_version);
  }

  // Delete leftover xcopy.log from old bat scripts (no longer generated).
  // The bat script now shows progress in a visible console window instead.
  {
    std::wstring xcopy_log = GetSelfDllDir() + L"\\xcopy.log";
    if (PathFileExistsW(xcopy_log.c_str())) {
      DeleteFileW(xcopy_log.c_str());
      AddDebugLog("Removed leftover xcopy.log");
    }
  }

  // Phase 2: If state is kPendingApply, move extracted files from temp dir
  // to Chrome directory. This is the fallback path — normally the bat script
  // handles this, but if the bat script failed to launch or didn't complete,
  // we handle it here on the next Chrome launch.
  {
    auto state = GetUpdateStateSnapshot();
    if (state.state == UpdateState::kPendingApply) {
      AddDebugLog("Detected kPendingApply state — checking for extracted files");

      std::wstring temp_dir = GetSelfDllDir() + L"\\update_temp";
      std::wstring chrome_bin = temp_dir + L"\\Chrome-bin";

      if (PathFileExistsW(chrome_bin.c_str())) {
        // Bat script didn't move the files — do it ourselves
        AddDebugLog("Moving extracted files to Chrome dir (fallback path)");
        MoveAllFiles(chrome_bin, GetAppDir());
        // RemoveDirectoryRecursive handles non-empty dirs (MoveAllFiles may
        // leave the source chrome.exe behind since it copies to .new).
        RemoveDirectoryRecursive(chrome_bin);
        RemoveDirectoryW(chrome_bin.c_str());
      }

      // Clean up temp dir entirely — use RemoveDirectoryRecursive because
      // RemoveDirectoryW only works on empty dirs. update_temp may still
      // contain Chrome-bin remnants, Setup.exe, chrome.7z, etc.
      std::wstring inner_7z = temp_dir + L"\\chrome.7z";
      DeleteFileW(inner_7z.c_str());
      RemoveDirectoryRecursive(temp_dir);
      RemoveDirectoryW(temp_dir.c_str());

      // NOTE: .old file cleanup is NOT done here. It's handled by the startup
      // cleanup block below, which runs on EVERY startup. We don't clean .old
      // files immediately after MoveAllFiles because:
      // 1. If MoveAllFiles renamed a file to .old and then failed to move the
      //    new file in, the .old file is the ONLY remaining copy. Deleting it
      //    would permanently lose the file (causing Chrome's 0x7E crash).
      //    MoveAllFiles now restores .old → original name on failure, but
      //    delaying cleanup provides an extra safety net.
      // 2. Antivirus software may be scanning newly-written files. Immediate
      //    cleanup could conflict with AV operations.

      // NOTE: Old version directory cleanup is NOT done here. It's handled by
      // the startup cleanup block below, which runs AFTER state is set to
      // kIdle. The cleanup block has its own safety checks to prevent deleting
      // the wrong version directory.

      // Check if chrome.exe.new exists — this means MoveAllFiles couldn't
      // replace the running chrome.exe and stashed the new one as .new.
      // In this case, the actual chrome.exe is STILL the old version, so
      // we must NOT set current_version = latest_version (that would be
      // wrong and could cause CleanupOldVersionDirs to delete the new dir).
      std::wstring new_exe = GetAppDir() + L"\\chrome.exe.new";
      bool has_new_exe = PathFileExistsW(new_exe.c_str());

      // Update state to idle
      {
        std::lock_guard<std::mutex> lock(g_update_mutex);
        g_update_state.state = UpdateState::kIdle;
        // Only update current_version if chrome.exe was actually replaced.
        // If .new exists, chrome.exe is still the old version — keep the
        // actual version (set at top of InitUpdater by GetInstalledChromeVersion).
        if (!has_new_exe) {
          g_update_state.current_version = g_update_state.latest_version;
        }
        g_update_state.download_progress = 0;
        g_update_state.downloaded_bytes = 0;
        g_update_state.download_path.clear();
        g_update_state.sha256.clear();
      }
      SaveUpdateState();
      AddDebugLog(has_new_exe
                  ? "Update partially applied (chrome.exe.new pending swap)"
                  : "Update applied successfully");

      // If MoveAllFiles created chrome.exe.new (because the running chrome.exe
      // couldn't be replaced), we need to swap it. We launch the standalone GUI
      // update window here and exit Chrome. The window waits for this process to
      // exit → copies chrome.exe.new → chrome.exe → cleans update_temp →
      // restarts Chrome with the new version.
      //
      // GUARD against infinite loops: this only runs in the kPendingApply path
      // (state was kPendingApply at startup). After the update window restarts
      // Chrome, state is kIdle, so this code is NOT re-entered. If the window
      // also fails to launch, .new persists and we retry on the next launch
      // (state stays kPendingApply, so InitUpdater re-enters this path).
      if (has_new_exe) {
        AddDebugLog("chrome.exe.new exists after fallback — launching GUI update "
                    "window to complete chrome.exe replacement");
        // The GUI update window waits for this Chrome to exit, then swaps
        // chrome.exe.new -> chrome.exe and relaunches Chrome.
        if (LaunchUpdateWindow()) {
          // Exit Chrome so the GUI updater can swap chrome.exe.new.
          // Delay 1s to let the updater process start and begin waiting.
          std::thread([]() {
            Sleep(1000);
            ExitProcess(0);
          }).detach();
          return;  // Don't continue InitUpdater — Chrome is about to exit
        }
        // The GUI window failed to launch. Leave state as kPendingApply so the
        // swap is retried on the next Chrome launch; continue InitUpdater.
        AddDebugLog("LaunchUpdateWindow failed for kPendingApply — will retry on next launch");
      }
    }
  }

  // Validate: if state is kReady but the downloaded file is missing,
  // reset to idle so the user isn't stuck at "ready to install" forever.
  {
    auto state = GetUpdateStateSnapshot();
    if (state.state == UpdateState::kReady) {
      std::wstring download_path_w = Utf8ToWstr(state.download_path);
      // Handle relative path (relative to DLL dir)
      if (download_path_w.find(L':') == std::wstring::npos) {
        download_path_w = GetSelfDllDir() + L"\\" + download_path_w;
      }
      if (!PathFileExistsW(download_path_w.c_str())) {
        AddDebugLog("Downloaded file missing, resetting stuck kReady state to idle");
        std::lock_guard<std::mutex> lock(g_update_mutex);
        g_update_state.state = UpdateState::kIdle;
        g_update_state.download_progress = 0;
        g_update_state.downloaded_bytes = 0;
        g_update_state.download_path.clear();
        g_update_state.sha256.clear();
      }
    }
  }

  // GetInstalledChromeVersion() was already called at the top of InitUpdater,
  // so current_version is authoritative. No need to call it again here.

  // Cleanup: always try to remove .old files on every startup (they may have
  // been left by a previous update where the locked file couldn't be deleted).
  // Also clean old version directories and installer files if configured.
  {
    // Clean .old files recursively in AppDir — they can appear in the root
    // (e.g. chrome.exe.old) or in version subdirectories (e.g. <version>\chrome_elf.dll.old)
    CleanupDotOldFiles(GetAppDir());

    // Migration: older builds stored chrome_green_update.json in the main
    // directory, and an intermediate build stored it in the "data" subdir.
    // State now lives in Chrome's data directory (<DLL dir>\..\Data), so remove
    // any orphaned copy from the previous locations to keep things clean.
    {
      std::wstring legacy_main = GetSelfDllDir() + L"\\chrome_green_update.json";
      if (PathFileExistsW(legacy_main.c_str())) {
        DeleteFileW(legacy_main.c_str());
      }
      std::wstring legacy_data = GetSelfDllDir() + L"\\data\\chrome_green_update.json";
      if (PathFileExistsW(legacy_data.c_str())) {
        DeleteFileW(legacy_data.c_str());
      }
      // Remove the now-empty intermediate "data" subdir if it has nothing else.
      // RemoveDirectoryW only succeeds on an empty dir, so this is a no-op
      // (and safe) if anything else lives there.
      RemoveDirectoryW((GetSelfDllDir() + L"\\data").c_str());
    }

    // Clean old version directories if keep_old_versions is off.
    // SAFETY: Skip cleanup if chrome.exe.new exists — the update swap hasn't
    // happened yet, and cleaning now could delete the new version's directory.
    // Also skip if chrome_ver != current_version (state mismatch means the
    // exe hasn't been swapped yet).
    if (!config.KeepOldVersions()) {
      std::wstring new_exe_check = GetAppDir() + L"\\chrome.exe.new";
      if (PathFileExistsW(new_exe_check.c_str())) {
        AddDebugLog("Skipping old version dir cleanup: chrome.exe.new exists (swap pending)");
      } else {
        auto snap = GetUpdateStateSnapshot();
        if (snap.current_version.empty() || chrome_ver == snap.current_version) {
          CleanupOldVersionDirs(GetAppDir(), chrome_ver);
        } else {
          AddDebugLog("Skipping old version dir cleanup: chrome_ver (" + chrome_ver +
                      ") != current_version (" + snap.current_version +
                      ") — chrome.exe not yet swapped");
        }
      }

      // Remove the "old" backup directory (created while keep_old_versions was
      // ON) once the option is turned OFF. The bat moves previous builds there;
      // the next Chrome startup detects it and deletes it. Only runs when the
      // option is off so an active backup is never wiped mid-use.
      std::wstring old_backup = GetAppDir() + L"\\old";
      if (PathFileExistsW(old_backup.c_str())) {
        RemoveDirectoryRecursive(old_backup);
        RemoveDirectoryW(old_backup.c_str());
        AddDebugLog("Removed old\\ backup directory (keep_old_versions is off)");
      }
    }

    // Clean up the updates/ folder on every startup.
    //   - Installer .7z/.exe packages are removed UNLESS keep_installer is
    //     ON (the user wants to retain them).
    // After files are removed, the updates/ directory itself is removed only
    // when keep_installer is OFF.
    {
      std::wstring updates_dir = GetSelfDllDir() + L"\\updates";
      std::wstring updates_search = updates_dir + L"\\*";
      WIN32_FIND_DATAW ufd;
      HANDLE hUFind = FindFirstFileW(updates_search.c_str(), &ufd);
      if (hUFind != INVALID_HANDLE_VALUE) {
        do {
          std::wstring name = ufd.cFileName;
          if (name == L"." || name == L"..") continue;
          std::wstring file_path = updates_dir + L"\\" + name;
          std::wstring ext = name.size() > 4 ? name.substr(name.size() - 4) : L"";
          // Always remove the transient GUI updater exe (it ends in .exe and
          // would otherwise be mistaken for a retained installer package when
          // keep_installer is ON). It is re-extracted on demand, never kept.
          if (_wcsnicmp(name.c_str(), L"chrome_green_updater", 19) == 0 &&
              _wcsicmp(ext.c_str(), L".exe") == 0) {
            // The transient GUI updater exe is image-locked while the GUI
            // process is still alive (~2-3s after it launches Chrome). Delete
            // now if possible; otherwise retry periodically (up to ~60s)
            // regardless of the error code. Once the GUI process exits the lock
            // is released and the next attempt succeeds. After the exe is gone,
            // remove the now-empty updates/ folder (unless keep_installer is ON).
            if (!DeleteFileW(file_path.c_str())) {
              std::thread([file_path, updates_dir]() {
                for (int i = 0; i < 60; i++) {
                  Sleep(1000);
                  if (DeleteFileW(file_path.c_str())) {
                    if (!config.KeepInstaller())
                      RemoveDirectoryW(updates_dir.c_str());
                    return;
                  }
                }
              }).detach();
            }
            continue;
          }
          // Keep installer packages only when keep_installer is ON.
          bool is_installer = (_wcsicmp(ext.c_str(), L".7z") == 0 ||
                              _wcsicmp(ext.c_str(), L".exe") == 0);
          if (is_installer && config.KeepInstaller()) {
            continue;
          }
          // Preserve the self_update staging directory when a self-update is
          // downloaded and waiting to be applied on restart.
          if ((ufd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
              _wcsicmp(name.c_str(), L"self_update") == 0) {
            auto state = GetUpdateStateSnapshot();
            if (state.self_update_ready) continue;
          }
          // Everything else (leftover installers when keep is off) is removed.
          if (ufd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveDirectoryRecursive(file_path);
          } else {
            DeleteFileW(file_path.c_str());
          }
        } while (FindNextFileW(hUFind, &ufd));
        FindClose(hUFind);
      }
      // Remove the updates/ directory only when the installer is not kept.
      // When keep_installer is ON the folder holds the retained package.
      if (!config.KeepInstaller()) {
        RemoveDirectoryW(updates_dir.c_str());
      }
    }

    // Always clean update_temp on every startup. It's a temporary extraction
    // directory that should never persist. The kPendingApply block above
    // already tries to clean it (after moving files out), but if that failed
    // (locked files, partial extraction), this is the safety net. On kIdle
    // startups (no pending update), this is the ONLY place update_temp is
    // cleaned — without this, a failed update leaves update_temp forever.
    {
      std::wstring temp_dir = GetSelfDllDir() + L"\\update_temp";
      if (PathFileExistsW(temp_dir.c_str())) {
        RemoveDirectoryRecursive(temp_dir);
        RemoveDirectoryW(temp_dir.c_str());
      }
    }
  }

  // Diagnostic: verify that the current version's chrome.dll exists and
  // is readable. CHECKS BOTH the true running exe version (GetInstalledChromeVersion)
  // AND the state.current_version (which kPendingApply may have overwritten).
  // Results go to the in-memory ring buffer (shown in the config page).
  {
    // Check TRUE running chrome.exe version's chrome.dll
    std::wstring true_dll = GetAppDir() + L"\\" + Utf8ToWstr(chrome_ver) + L"\\chrome.dll";
    bool true_readable = false;
    if (PathFileExistsW(true_dll.c_str())) {
      HANDLE h = CreateFileW(true_dll.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); true_readable = true; }
      else {
        DWORD err = GetLastError();
        AddDebugLog("TRUE chrome.dll (exe ver) LOCKED: " + std::string(true_dll.begin(), true_dll.end()) +
                    " error=" + std::to_string(err));
      }
    } else {
      AddDebugLog("CRITICAL: TRUE chrome.dll MISSING: " + std::string(true_dll.begin(), true_dll.end()));
    }
    AddDebugLog(std::string("chrome.dll (exe=") + chrome_ver + ") readable: " + (true_readable ? "YES" : "NO"));

    // Also check state.current_version path (may differ after kPendingApply)
    auto state = GetUpdateStateSnapshot();
    if (state.current_version != chrome_ver) {
      std::wstring state_dll = GetAppDir() + L"\\" + Utf8ToWstr(state.current_version) + L"\\chrome.dll";
      bool state_readable = false;
      if (PathFileExistsW(state_dll.c_str())) {
        HANDLE h = CreateFileW(state_dll.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); state_readable = true; }
      }
      AddDebugLog(std::string("chrome.dll (state=") + state.current_version + ") readable: " + (state_readable ? "YES" : "NO"));
    }
  }

  // Determine architecture from the current process
  g_update_state.arch = Config::DetectArch();

  // Load config values into state
  g_update_state.channel = config.GetUpdateChannel();
  g_update_state.auto_check = config.IsAutoCheck();
  g_update_state.auto_download = config.IsAutoDownload();
  g_update_state.proxy = config.GetUpdateProxy();
  g_update_state.proxy_type = config.GetUpdateProxyType();
  g_update_state.proxy_chrome_download = config.ProxyChromeDownload();
  g_update_state.download_source = config.GetDownloadSource();
  // keep_installer and keep_old_versions are read on demand from config
  SaveUpdateState();

  // Start the HTTP server for the config page (port reserved earlier)
  StartHttpServer();

  // Auto-check for updates if enabled and enough time has passed
  if (config.IsAutoCheck()) {
    int interval_hours = config.GetCheckInterval();
    if (interval_hours <= 0) interval_hours = 24;

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t interval_seconds = interval_hours * 3600;

    if (g_update_state.last_check_time == 0 ||
        (now - g_update_state.last_check_time) >= interval_seconds) {
      // Delay check by 5 seconds to let Chrome fully start
      std::thread([]() {
        Sleep(5000);
        TriggerUpdateCheck();
      }).detach();
    }
  }
}

void ShutdownUpdater() {
  StopHttpServer();
  if (check_thread_.joinable()) {
    check_thread_.join();
  }
}
