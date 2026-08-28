#include <windows.h>

#include <psapi.h>
#include <shobjidl.h>
#include <string>

#include "detours.h"

#include "actionhotkey.h"
#include "appid.h"
#include "config.h"
#include "green.h"
#include "hijack.h"
#include "hosts_manager.h"
#include "hotkey.h"
#include "inputhook.h"
#include "keycapture.h"
#include "keymapping.h"
#include "pakpatch.h"
#include "httpserver.h"
#include "policies.h"
#include "portable.h"
#include "tabbookmark.h"
#include "tools.h"
#include "upgradenotification.h"
#include "updater.h"
#include "utils.h"
#include "version.h"

using Startup = int (*)();
static bool should_run_exit_cmd = false;
Startup ExeMain = nullptr;

void ChromeGreen() {
  // Force Chrome's AppUserModelID to our install-unique id and register the
  // matching icon, so the taskbar button groups with pinned shortcuts.
  SetAppId();

  // Portable hijack patch.
  MakeGreen();

  // Ignore enterprise policies.
  IgnorePolicies();

  // Initialize key mapping and translate key
  KeyMapping();

  // Register the boss-key hotkey (global, works even when Chrome is hidden).
  GetHotkey();

  // Enhancement of the tab bar / bookmarks (ported from chrome_plus).
  // Must run before InstallInputHooks() so its handlers are registered first.
  TabBookmark();

  // Action hotkeys: configured shortcuts that open a new window or batch-open
  // a list of URLs as tabs. Must run before InstallInputHooks().
  ActionHotkeys();

  // Backend-assisted hotkey capture: a highest-priority keyboard handler that
  // swallows keys while the config page is recording, so Chrome's own
  // accelerators never steal the capture. Must run before InstallInputHooks().
  InitKeyCapture();

  // Install input hooks (must be called after all handlers are registered).
  InstallInputHooks();

  // Reserve a per-install config port BEFORE patching the pak, so the
  // config-page links baked into the browser UI match the port the HTTP
  // server will actually bind (and survive an occupied base port).
  ReserveConfigServerPort();

  // Patch the pak file.
  PakPatch();

  // Suppress Chrome's false "out of date" upgrade notification.
  SuppressFalseUpgradeNotification();

  // Initialize the updater (HTTP server + auto-check).
  InitUpdater();

  // Start the background domain-mapping (域名映射) subscription refresh thread.
  resolver::StartResolverRefresh();
}

void ChromeGreenCommand(LPWSTR param) {
  if (!wcsstr(param, L"--portable")) {
    Portable(param);
  } else {
    // Launched with --portable (pinned item / desktop shortcut): make sure the
    // configured portable paths (--user-data-dir / --disk-cache-dir) are on
    // THIS process's command line too. Without this the shortcut would use the
    // default (non-portable) profile and the portable identity would be lost.
    ApplyPortableArgsToCommandLine();
    ChromeGreen();
    LaunchCommands(config.GetLaunchOnStartup());
    should_run_exit_cmd = true;
  }
}

int Loader() {
  // Only main interface.
  LPWSTR param = GetCommandLineW();
  // DebugLog(L"param {}", param);
  if (!wcsstr(param, L"-type=")) {
    // Apply pending update before Chrome starts (if one is ready).
    ApplyPendingUpdate();
    ChromeGreenCommand(param);
  } else if (wcsstr(param, L"--type=renderer")) {
    // With in-process WebUI resource loading on (V1, crrev.com/c/5868139,
    // crbug.com/362511750), the browser fills `LocalResourceLoaderConfig`
    // (content/browser/webui/web_ui_impl.cc) and the renderer materializes
    // WebUI from its own inherited-handle `resources.pak`
    // (ui/base/resource/data_pack.cc), not over IPC. So the renderer must
    // patch that mapping itself to keep the injection from #172. We stopped
    // forcing V1 off because the field trial enabling
    // `InitialWebUISyncNavStartToCommit` (crrev.com/c/7778247) then made the
    // renderer CHECK that config (content/renderer/render_frame_impl.cc) and
    // crash (#263). Other sub-process types never serve WebUI, so skip them.
    PakPatch();
  }

  // Return to the main function.
  int r = ExeMain();
  return r;
}

void InstallLoader() {
  // Get the address of the original entry point of the main module.
  MODULEINFO mi;
  GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &mi,
                       sizeof(MODULEINFO));
  ExeMain = reinterpret_cast<Startup>(mi.EntryPoint);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<LPVOID*>(&ExeMain),
               reinterpret_cast<void*>(Loader));
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"InstallLoader failed: {}", status);
  }
}

__declspec(dllexport) void portable() {}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID pv) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    hInstance = hModule;

    // Maintain the original function of system DLLs.
    LoadSysDll(hModule);

    InstallLoader();
  } else if (dwReason == DLL_PROCESS_DETACH && ::should_run_exit_cmd) {
    ShutdownUpdater();
    LaunchCommands(config.GetLaunchOnExit());
    should_run_exit_cmd = false;
  }
  return TRUE;
}
