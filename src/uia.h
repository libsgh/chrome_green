#ifndef CHROME_GREEN_SRC_UIA_H_
#define CHROME_GREEN_SRC_UIA_H_

#include <uiautomation.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

struct TabHitResult {
  Microsoft::WRL::ComPtr<IUIAutomationElement> tab;
  int tab_count = 0;
  bool on_close_button = false;
};

[[nodiscard]] std::optional<TabHitResult>
FindTabHitResult(POINT pt, bool need_count, bool need_close_button);
// Deliberately not [[nodiscard]]: the caller has no fallback action;
// failures are logged at the failure site.
bool SelectTab(const TabHitResult& hit_result);
[[nodiscard]] std::optional<int> FindTabCount(HWND hwnd);
[[nodiscard]] bool IsOnTabBar(POINT pt);
[[nodiscard]] bool IsOnBookmark(POINT pt);
[[nodiscard]] bool IsOmniboxFocused();
[[nodiscard]] bool IsOnNewTab(HWND hwnd,
                              const std::vector<std::wstring>& extra_tab_names);

// Screen rectangle of the tab strip, for hit tests outside a mouse-move loop
// (resolving it per point during an OLE drag is far too costly).
[[nodiscard]] std::optional<RECT> GetTabStripRect(HWND hwnd);
// Bookmark-bar zone of one browser window in screen coordinates: the omnibox
// row, plus the bookmark bar row while the bar is visible.
struct BookmarkBarUi {
  RECT toolbar_rect{};
  std::optional<RECT> bar_rect;
};

[[nodiscard]] std::optional<BookmarkBarUi> GetBookmarkBarUi(HWND hwnd);
[[nodiscard]] bool IsBookmarkBarVisible(HWND hwnd);
[[nodiscard]] bool IsOnBookmarkBarZone(POINT pt);
// Drop the cached resolution; call after toggling the bar so the next query
// re-resolves whether it is on screen.
void InvalidateBookmarkBarUi();

#endif  // CHROME_GREEN_SRC_UIA_H_
