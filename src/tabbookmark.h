#ifndef CHROME_GREEN_SRC_TABBOOKMARK_H_
#define CHROME_GREEN_SRC_TABBOOKMARK_H_

#include <windows.h>

void TabBookmark();

// Reconciles the bookmark bar with the current config. Call after
// Config::ReloadConfig() — the mouse hook alone misses a switch flipped while
// the pointer sits still.
void SyncBookmarkBarWithConfig();

#endif  // CHROME_GREEN_SRC_TABBOOKMARK_H_
