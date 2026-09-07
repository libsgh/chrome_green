#ifndef CHROME_GREEN_SRC_APPID_H_
#define CHROME_GREEN_SRC_APPID_H_

// Captures Chrome's own AppUserModelID (via SHGetPropertyStoreForWindow hook)
// and registers corrected jump-list Tasks (New Window / Incognito) that point
// to THIS install's chrome.exe under that same AUMID.  Uses pass-through mode
// (no forced custom AUMID) so pinned shortcuts and the running process share
// one taskbar identity → single icon.
void SetAppId();

// Stable per-install port for the local config HTTP server. Derived from the
// install directory so multiple portable Chromes each get their own port
// instead of all colliding on a single hardcoded port (which would route one
// instance's config page / update actions to another instance's server).
int GetConfigPort();

#endif  // CHROME_GREEN_SRC_APPID_H_
