#ifndef CHROME_GREEN_SRC_TOOLS_H_
#define CHROME_GREEN_SRC_TOOLS_H_

#include <string>

// Result of a tool action.
struct ToolsResult {
  bool success = false;   // operation completed without error
  std::wstring message;   // UTF-16 status/error text (for debug logs)
  bool done = false;      // resulting state: shortcut exists
};

// Create a desktop shortcut to the portable Chrome (chrome.exe --portable).
// If a "Google Chrome.lnk" already exists on the desktop, a parenthesized
// index is used (Google Chrome (2).lnk, ...).
ToolsResult CreateDesktopShortcut();

// Detection helper for the config-page desktop-shortcut indicator.
bool DesktopShortcutExists();

// Status of the default (non-portable) Chrome data directory:
//   C:\Users\<user>\AppData\Local\Google\Chrome
struct ChromeDataStatus {
  bool exists = false;      // directory is present on disk
  bool empty = true;        // not present, or no direct children
  int entry_count = 0;      // number of direct children
};

// Inspect the default Chrome data directory (for the config-page indicator).
ChromeDataStatus GetChromeDefaultDataStatus();

// Remove the default Chrome data directory entirely. The path is guarded so
// only the exact "...\Google\Chrome" location can ever be removed.
ToolsResult CleanChromeDefaultData();

// Convert a wide string to UTF-8 (for JSON message fields).
std::string ToUtf8(const std::wstring& s);

#endif  // CHROME_GREEN_SRC_TOOLS_H_
