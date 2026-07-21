#include "actionhotkey.h"

#include <windows.h>

#include <string>
#include <vector>

#include "config.h"
#include "inputhook.h"
#include "utils.h"

namespace {

struct ActionHotkey {
  UINT modifiers = 0;
  UINT vk = 0;
};

ActionHotkey open_new_window;
ActionHotkey open_url_group;

// Exact modifier match: every modifier in `modifiers` must be held, and no
// extra modifier (beyond those specified) may be held.
bool CheckModifiers(UINT modifiers) {
  const bool shift_ok = !(modifiers & MOD_SHIFT) || IsKeyPressed(VK_SHIFT);
  const bool ctrl_ok = !(modifiers & MOD_CONTROL) || IsKeyPressed(VK_CONTROL);
  const bool alt_ok = !(modifiers & MOD_ALT) || IsKeyPressed(VK_MENU);
  const bool win_ok =
      !(modifiers & MOD_WIN) || IsKeyPressed(VK_LWIN) || IsKeyPressed(VK_RWIN);

  const bool no_extra_shift =
      (modifiers & MOD_SHIFT) || !IsKeyPressed(VK_SHIFT);
  const bool no_extra_ctrl =
      (modifiers & MOD_CONTROL) || !IsKeyPressed(VK_CONTROL);
  const bool no_extra_alt = (modifiers & MOD_ALT) || !IsKeyPressed(VK_MENU);
  const bool no_extra_win = (modifiers & MOD_WIN) ||
                            (!IsKeyPressed(VK_LWIN) && !IsKeyPressed(VK_RWIN));

  return shift_ok && ctrl_ok && alt_ok && win_ok && no_extra_shift &&
         no_extra_ctrl && no_extra_alt && no_extra_win;
}

void OpenNewWindow() {
  ExecuteCommand(IDC_NEW_WINDOW);
}

// Open each configured URL in its own new tab by launching the current
// Chrome executable with the URL as its argument. Since Chrome is already
// running, this reuses the existing process and opens a new tab in the
// current window instead of relying on keyboard input to the omnibox.
void OpenUrlGroup() {
  wchar_t exe_path[MAX_PATH] = {};
  if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
    return;
  }

  for (const auto& url : config.GetUrlGroup()) {
    if (url.empty()) {
      continue;
    }
    ::ShellExecuteW(nullptr, L"open", exe_path, url.c_str(), nullptr,
                    SW_SHOWNORMAL);
    // Small delay so Chrome processes each URL sequentially and avoids
    // creating multiple new windows when launched in rapid succession.
    ::Sleep(120);
  }
}

bool ActionHotkeyHandler(WPARAM wParam, LPARAM lParam) {
  // Ignore key-up events.
  if (lParam & 0x80000000) {
    return false;
  }

  // These actions only make sense when a Chrome window is in the foreground.
  if (!IsChromeWindow(GetForegroundWindow())) {
    return false;
  }

  if (open_new_window.vk != 0 && wParam == open_new_window.vk &&
      CheckModifiers(open_new_window.modifiers)) {
    OpenNewWindow();
    return true;
  }

  if (open_url_group.vk != 0 && wParam == open_url_group.vk &&
      CheckModifiers(open_url_group.modifiers)) {
    OpenUrlGroup();
    return true;
  }

  return false;
}

void ParseActionHotkey(const std::wstring& str, ActionHotkey& out) {
  if (str.empty()) {
    return;
  }
  // no_repeat=false so MOD_NOREPEAT is not added (matches keymapping parsing).
  const UINT parsed = ParseHotkeys(str, /*no_repeat=*/false);
  out.modifiers = LOWORD(parsed);
  out.vk = HIWORD(parsed);
}

}  // namespace

void ActionHotkeys() {
  ParseActionHotkey(config.GetOpenNewWindowHotkey(), open_new_window);
  ParseActionHotkey(config.GetOpenUrlGroupHotkey(), open_url_group);

  if (open_new_window.vk != 0 || open_url_group.vk != 0) {
    RegisterKeyboardHandler(ActionHotkeyHandler, HandlerPriority::kHigh);
    DebugLog(L"ActionHotkeys: registered (new_window={}, url_group={})",
             open_new_window.vk, open_url_group.vk);
  }
}
