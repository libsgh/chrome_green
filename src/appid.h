#ifndef CHROME_GREEN_SRC_APPID_H_
#define CHROME_GREEN_SRC_APPID_H_

#include <string>

// Installs Detours hooks that FORCE Chrome's AppUserModelID (process, windows,
// and property store) to our install-unique "ChromeGreen.<hash>" id, and
// registers a matching icon, so the taskbar button groups with any pinned
// shortcut that carries the same id. The jump-list menu is left to Chrome's
// default (no custom tasks). Idempotent.
void SetAppId();

// Install-unique AppUserModelID, computed purely from the install directory:
// "ChromeGreen." + uppercase hex FNV-1a 32-bit hash of the full AppDir path.
// Used both to force onto Chrome and to stamp pinned shortcuts.
std::wstring GetGreenAumid();

// Stable per-install port for the local config HTTP server. Derived from the
// install directory so multiple portable Chromes each get their own port
// instead of all colliding on a single hardcoded port (which would route one
// instance's config page / update actions to another instance's server).
int GetConfigPort();

#endif  // CHROME_GREEN_SRC_APPID_H_
