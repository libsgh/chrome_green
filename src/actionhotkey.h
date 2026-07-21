#ifndef CHROME_GREEN_SRC_ACTIONHOTKEY_H_
#define CHROME_GREEN_SRC_ACTIONHOTKEY_H_

// Register the "action hotkeys" feature: user-configured shortcut keys that
// trigger a Chrome action (open a new window, or batch-open a list of URLs as
// tabs). Wired from ChromeGreen() before InstallInputHooks().
void ActionHotkeys();

#endif  // CHROME_GREEN_SRC_ACTIONHOTKEY_H_
