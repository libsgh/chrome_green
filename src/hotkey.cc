#include "hotkey.h"

#include <windows.h>

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "com_initializer.h"
#include "config.h"
#include "utils.h"

namespace {

using Microsoft::WRL::ComPtr;
using HotkeyAction = void (*)();

// Static variables for internal use
bool is_hide = false;
std::vector<HWND> hwnd_list;
std::unordered_map<std::wstring, bool> original_mute_states;

BOOL CALLBACK SearchChromeWindow(HWND hwnd, LPARAM lparam) {
  if (IsWindowVisible(hwnd) && IsChromeWindow(hwnd)) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
      ShowWindow(hwnd, SW_HIDE);
      hwnd_list.emplace_back(hwnd);
    }
  }
  return true;
}

std::wstring GetProcessImagePath(DWORD pid) {
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return {};
  }
  wchar_t buffer[MAX_PATH];
  DWORD size = MAX_PATH;
  std::wstring path;
  if (::QueryFullProcessImageNameW(process, 0, buffer, &size)) {
    path.assign(buffer, size);
  }
  ::CloseHandle(process);
  return path;
}

// Process ids of the very same executable file as this process.
//
// Matching on the file name alone (the old behaviour) also picked up every
// other Chrome installation on the machine — a system Chrome, a second
// portable copy, ... — so the boss key muted browsers it must never touch.
// Those foreign instances are also the ones whose persisted mute state we can
// never restore, since the flag file below only tracks OUR instance.
std::vector<DWORD> GetAppPids() {
  std::vector<DWORD> pids;
  wchar_t current_exe_path[MAX_PATH];
  if (!::GetModuleFileNameW(nullptr, current_exe_path, MAX_PATH)) {
    return pids;
  }
  std::wstring self_path(current_exe_path);
  wchar_t* exe_name = wcsrchr(current_exe_path, L'\\');
  const wchar_t* self_exe_name = exe_name ? exe_name + 1 : current_exe_path;

  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return pids;
  }

  PROCESSENTRY32W pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32W);

  if (::Process32FirstW(snapshot, &pe32)) {
    do {
      if (_wcsicmp(pe32.szExeFile, self_exe_name) != 0) {
        continue;
      }
      if (pe32.th32ProcessID == 0) {
        continue;
      }
      // Same file name is not enough: compare the full image path. When the
      // path cannot be read (protected process), skip it rather than muting a
      // process that may well be a different Chrome.
      std::wstring path = GetProcessImagePath(pe32.th32ProcessID);
      if (path.empty() || _wcsicmp(path.c_str(), self_path.c_str()) != 0) {
        continue;
      }
      pids.emplace_back(pe32.th32ProcessID);
    } while (::Process32NextW(snapshot, &pe32));
  }

  ::CloseHandle(snapshot);
  return pids;
}

std::optional<std::wstring> GetSessionKey(IAudioSessionControl2* session2) {
  if (!session2) {
    return std::nullopt;
  }
  LPWSTR session_id = nullptr;
  if (SUCCEEDED(session2->GetSessionInstanceIdentifier(&session_id)) &&
      session_id) {
    std::wstring key(session_id);
    CoTaskMemFree(session_id);
    return key;
  }
  return std::nullopt;
}

// Returns true when this session belonged to one of `pids` and was
// (un)muted by this call.
bool ProcessAudioSession(IAudioSessionControl* session,
                         const std::vector<DWORD>& pids,
                         bool set_mute,
                         bool save_mute_state,
                         bool force_unmute) {
  ComPtr<IAudioSessionControl2> session2;
  if (FAILED(session->QueryInterface(IID_PPV_ARGS(&session2)))) {
    return false;
  }

  DWORD session_pid = 0;
  if (FAILED(session2->GetProcessId(&session_pid))) {
    return false;
  }

  bool pid_matches = false;
  for (DWORD p : pids) {
    if (p == session_pid) {
      pid_matches = true;
      break;
    }
  }
  if (!pid_matches) {
    return false;
  }

  ComPtr<ISimpleAudioVolume> volume;
  if (FAILED(session2->QueryInterface(IID_PPV_ARGS(&volume)))) {
    return false;
  }

  auto session_key = GetSessionKey(session2.Get());

  if (save_mute_state && session_key) {
    BOOL is_muted = FALSE;
    if (SUCCEEDED(volume->GetMute(&is_muted))) {
      original_mute_states[*session_key] = (is_muted == TRUE);
    }
  }

  if (set_mute) {
    volume->SetMute(TRUE, nullptr);
    return true;
  }

  // Unmute logic:
  // - If we have recorded state for this session, respect it
  // - If session is new (not in our records), unmute it
  //   (it was likely created after hide, so it should be unmuted)
  // - `force_unmute` (self-healing a stale persisted mute) always unmutes
  bool should_unmute = force_unmute;
  if (!should_unmute) {
    should_unmute = true;
    if (session_key) {
      auto state_it = original_mute_states.find(*session_key);
      if (state_it != original_mute_states.end()) {
        should_unmute = !state_it->second;  // unmute only if was not muted
      }
    }
  }
  if (should_unmute) {
    volume->SetMute(FALSE, nullptr);
    return true;
  }
  return false;
}

// Returns the number of sessions that were actually (un)muted.
//
// Every ACTIVE render endpoint is walked, not just the current default one:
// the mute flag is persisted per (device, executable) pair, so muting while
// the default device was e.g. the speakers left the headphone entry untouched
// and vice versa — a device switch then "lost" the restore.
int MuteProcess(const std::vector<DWORD>& pids,
                bool set_mute,
                bool save_mute_state = false,
                bool force_unmute = false) {
  ComInitializer com;
  if (!com.IsInitialized()) {
    return 0;
  }

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return 0;
  }

  ComPtr<IMMDeviceCollection> devices;
  if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                            &devices))) {
    return 0;
  }

  UINT device_count = 0;
  if (FAILED(devices->GetCount(&device_count))) {
    return 0;
  }

  int hits = 0;
  for (UINT d = 0; d < device_count; ++d) {
    ComPtr<IMMDevice> device;
    if (FAILED(devices->Item(d, &device)) || !device) {
      continue;
    }

    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(
            __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(manager.GetAddressOf())))) {
      continue;
    }

    ComPtr<IAudioSessionEnumerator> session_enumerator;
    if (FAILED(manager->GetSessionEnumerator(&session_enumerator))) {
      continue;
    }

    int session_count = 0;
    if (FAILED(session_enumerator->GetCount(&session_count))) {
      continue;
    }

    for (int i = 0; i < session_count; ++i) {
      ComPtr<IAudioSessionControl> session;
      if (SUCCEEDED(session_enumerator->GetSession(i, &session)) && session) {
        if (ProcessAudioSession(session.Get(), pids, set_mute, save_mute_state,
                                force_unmute)) {
          ++hits;
        }
      }
    }
  }
  return hits;
}

// ---------------------------------------------------------------------------
// Persisted "boss key left us muted" flag
// ---------------------------------------------------------------------------
// `ISimpleAudioVolume::SetMute()` is NOT a transient in-memory toggle: Windows
// persists it per (device, executable path) and replays it onto every new
// session of that executable — which is why the mute survived a Chrome update
// and a fresh `version.dll`. So a Chrome that dies, is killed or is restarted
// by the self-updater while the boss key is hidden never runs the restore
// branch (`is_hide` is a plain in-memory bool that comes back as `false`),
// leaving the browser permanently silent with no way to tell why.
//
// The flag file lets the next launch detect "we muted ourselves and never
// unmuted" and heal it.
constexpr wchar_t kMutedFlagFileName[] = L"chrome_green_boss_muted";

std::wstring GetMutedFlagPath() {
  return GetAppDir() + L"\\" + kMutedFlagFileName;
}

bool HasMutedFlag() {
  return ::GetFileAttributesW(GetMutedFlagPath().c_str()) !=
         INVALID_FILE_ATTRIBUTES;
}

void SetMutedFlag(bool muted) {
  const std::wstring path = GetMutedFlagPath();
  if (muted) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      ::CloseHandle(file);
    }
  } else {
    ::DeleteFileW(path.c_str());
  }
}

// Unmute sessions left over from a previous hidden-and-lost launch.
void RestoreMutedOnStartup() {
  if (!HasMutedFlag()) {
    return;
  }
  DebugLog(L"boss key: stale mute flag found, restoring audio");
  std::thread([] {
    // Chrome only creates an audio session when it actually starts playing,
    // so shortly after launch there is nothing to unmute yet. Poll for a
    // while instead of giving up on the first empty enumeration.
    for (int attempt = 0; attempt < 60; ++attempt) {
      int hits = MuteProcess(GetAppPids(), /*set_mute=*/false,
                             /*save_mute_state=*/false, /*force_unmute=*/true);
      if (hits > 0) {
        DebugLog(L"boss key: restored audio on {} session(s)", hits);
        // One more pass: the persisted flag is rewritten asynchronously, so
        // make sure it really landed as "unmuted".
        ::Sleep(1000);
        MuteProcess(GetAppPids(), /*set_mute=*/false,
                    /*save_mute_state=*/false, /*force_unmute=*/true);
        break;
      }
      ::Sleep(3000);
    }
    SetMutedFlag(false);
  }).detach();
}

void HideAndShow() {
  auto chrome_pids = GetAppPids();
  if (!is_hide) {
    original_mute_states.clear();
    EnumWindows(SearchChromeWindow, 0);
    MuteProcess(chrome_pids, true, true);
    SetMutedFlag(true);
  } else {
    for (auto r_iter = hwnd_list.rbegin(); r_iter != hwnd_list.rend();
         ++r_iter) {
      ShowWindow(*r_iter, SW_SHOW);
      SetWindowPos(*r_iter, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
      SetForegroundWindow(*r_iter);
      SetWindowPos(*r_iter, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE);
      SetActiveWindow(*r_iter);
    }
    hwnd_list.clear();
    MuteProcess(chrome_pids, false);
    original_mute_states.clear();
    SetMutedFlag(false);
  }
  is_hide = !is_hide;
}

void OnHotkey(HotkeyAction action) {
  action();
}

void Hotkey(std::wstring_view keys, HotkeyAction action) {
  if (keys.empty()) {
    return;
  } else {
    UINT flag = ParseHotkeys(keys.data());

    std::thread th([flag, action]() {
      RegisterHotKey(nullptr, 0, LOWORD(flag), HIWORD(flag));

      MSG msg;
      while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
          OnHotkey(action);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    });
    th.detach();
  }
}

}  // namespace

void GetHotkey() {
  // Heal a mute that a previous (hidden) session left behind, even if the
  // boss key has since been removed from the config.
  RestoreMutedOnStartup();

  const auto& boss_key = config.GetBossKey();
  if (!boss_key.empty()) {
    Hotkey(boss_key, HideAndShow);
  }
}
