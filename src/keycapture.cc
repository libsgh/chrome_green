#include "keycapture.h"

#include <windows.h>

#include <string>
#include <vector>

#include "inputhook.h"
#include "utils.h"

namespace {

bool g_capturing = false;

// Pending SSE payload (JSON), consumed by the stream loop. Guarded so the
// hook thread (producer) and the HTTP stream thread (consumer) never race.
CRITICAL_SECTION g_cs;
std::string g_pending_capture;

bool IsModifierVk(UINT vk) {
  switch (vk) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LWIN:
    case VK_RWIN:
      return true;
    default:
      return false;
  }
}

// Map a virtual key to the display/parse name used by ParseHotkeys.
std::wstring KeyNameFromVk(UINT vk) {
  if (vk >= VK_F1 && vk <= VK_F24) {
    return L"F" + std::to_wstring(vk - VK_F1 + 1);
  }
  switch (vk) {
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_ESCAPE: return L"Esc";
    case VK_TAB: return L"Tab";
    case VK_BACK: return L"Backspace";
    case VK_RETURN: return L"Enter";
    case VK_SPACE: return L"Space";
    case VK_SNAPSHOT: return L"PrintScreen";
    case VK_SCROLL: return L"Scroll";
    case VK_PAUSE: return L"Pause";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_CAPITAL: return L"CapsLock";
    case VK_NUMLOCK: return L"NumLock";
    case VK_APPS: return L"Apps";
    default: break;
  }
  // Printable character via the virtual-key -> character mapping.
  wchar_t ch = static_cast<wchar_t>(::MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR));
  if (ch != 0) {
    return std::wstring(1, ch);
  }
  return L"VK" + std::to_wstring(vk);
}

std::wstring BuildCombo(UINT vk) {
  std::wstring combo;
  if (IsKeyPressed(VK_CONTROL)) combo += L"Ctrl+";
  if (IsKeyPressed(VK_MENU)) combo += L"Alt+";
  if (IsKeyPressed(VK_SHIFT)) combo += L"Shift+";
  if (IsKeyPressed(VK_LWIN) || IsKeyPressed(VK_RWIN)) combo += L"Win+";
  combo += KeyNameFromVk(vk);
  return combo;
}

void Broadcast(const std::string& json) {
  ::EnterCriticalSection(&g_cs);
  g_pending_capture = json;
  ::LeaveCriticalSection(&g_cs);
}

// Highest-priority keyboard handler. Active only while capturing; swallows
// every key so Chrome never reacts, and reports the captured combination.
bool CaptureHandler(WPARAM wParam, LPARAM lParam) {
  if (!g_capturing) {
    return false;
  }

  const bool key_up = (lParam & 0x80000000) != 0;
  const UINT vk = static_cast<UINT>(wParam);

  // Esc cancels the capture (no value recorded).
  if (vk == VK_ESCAPE && !key_up) {
    Broadcast("{\"cancel\":true}");
    g_capturing = false;
    return true;
  }

  // Swallow key-up events too so nothing leaks through during capture.
  if (key_up) {
    return true;
  }

  // Wait for a non-modifier key to finalize the combination. This lets the
  // user hold Ctrl/Alt/Shift/Win first and then press the main key.
  if (IsModifierVk(vk)) {
    return true;
  }

  const std::wstring combo = BuildCombo(vk);
  Broadcast("{\"value\":\"" + WStringToUtf8(combo) + "\"}");
  g_capturing = false;
  return true;
}

}  // namespace

void StartKeyCapture() {
  g_capturing = true;
}

void StopKeyCapture() {
  g_capturing = false;
}

void InitKeyCapture() {
  ::InitializeCriticalSection(&g_cs);
  RegisterKeyboardHandler(CaptureHandler, HandlerPriority::kHighest);
}

bool ConsumeKeyCaptureEvent(std::string& out) {
  ::EnterCriticalSection(&g_cs);
  if (g_pending_capture.empty()) {
    ::LeaveCriticalSection(&g_cs);
    return false;
  }
  out = std::move(g_pending_capture);
  g_pending_capture.clear();
  ::LeaveCriticalSection(&g_cs);
  return true;
}
