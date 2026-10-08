#include "dragdrop.h"

#include <windows.h>

#include <ole2.h>

#include <string>
#include <string_view>

#include "detours.h"

#include "config.h"
#include "uia.h"
#include "utils.h"

namespace {

auto RawRegisterDragDrop = RegisterDragDrop;

// Registered clipboard formats run from CF_PRIVATEFIRST up to 0xFFFF. The SDK
// has no CF_LAST.
constexpr UINT kMaxRegisteredFormat = 0xFFFF;

bool IsRegisteredFormat(UINT format) {
  return format >= CF_PRIVATEFIRST && format <= kMaxRegisteredFormat;
}

// Formats that mark a drag as a link. Pages add their own types on top, so this
// is a floor, not a whitelist.
bool IsLinkFormat(const FORMATETC& format) {
  switch (format.cfFormat) {
    case CF_UNICODETEXT:
    case CF_TEXT:
    case CF_LOCALE:
      return true;
    default:
      break;
  }
  if (!IsRegisteredFormat(format.cfFormat)) {
    return false;
  }
  wchar_t name[64]{};
  const int length = GetClipboardFormatNameW(format.cfFormat, name,
                                             static_cast<int>(ARRAYSIZE(name)));
  if (length <= 0) {
    return false;
  }
  const std::wstring_view view(name, static_cast<size_t>(length));
  return view == L"text/uri-list" || view == L"text/plain" ||
         view == L"text/html";
}

// Requires a link format to be present; requiring every format to match used to
// reject genuine link drags, since Chromium appends internal ones.
bool IsLinkDrag(IDataObject* data_object) {
  if (!data_object) {
    return false;
  }

  IEnumFORMATETC* enumerator = nullptr;
  if (FAILED(data_object->EnumFormatEtc(DATADIR_GET, &enumerator)) ||
      !enumerator) {
    return false;
  }

  bool has_link = false;
  bool has_files = false;
  FORMATETC format{};
  while (enumerator->Next(1, &format, nullptr) == S_OK) {
    if (format.cfFormat == CF_HDROP) {
      has_files = true;
      break;
    }
    has_link = has_link || IsLinkFormat(format);
  }

  enumerator->Release();
  return has_link && !has_files;
}

// Reads the dragged URL, preferring CF_UNICODETEXT and falling back to
// text/uri-list, whose payload is "<url>\r\n".
bool ReadDraggedUrl(IDataObject* data_object, std::wstring& url) {
  if (!data_object) {
    return false;
  }

  FORMATETC request{};
  request.dwAspect = DVASPECT_CONTENT;
  request.lindex = -1;
  request.tymed = TYMED_HGLOBAL;

  // QueryGetData is the availability probe; GetData rejects a null STGMEDIUM.
  request.cfFormat = CF_UNICODETEXT;
  if (FAILED(data_object->QueryGetData(&request))) {
    request.cfFormat = RegisterClipboardFormatW(L"text/uri-list");
    if (request.cfFormat == 0) {
      return false;
    }
  }

  STGMEDIUM medium{};
  if (FAILED(data_object->GetData(&request, &medium))) {
    return false;
  }
  if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) {
    ReleaseStgMedium(&medium);
    return false;
  }

  if (const auto* text =
          static_cast<const wchar_t*>(GlobalLock(medium.hGlobal))) {
    url.assign(text);
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);

  while (!url.empty() &&
         (url.back() == L'\r' || url.back() == L'\n' || url.back() == L' ')) {
    url.pop_back();
  }
  return !url.empty();
}

// Reading IDataObject ourselves bypasses Chromium's FilterDropData, which is
// what normally stops a page from dropping a chrome:// URL into the browser.
bool IsAllowedScheme(std::wstring_view url) {
  return url.starts_with(L"http://") || url.starts_with(L"https://");
}

// The URL itself is only needed to validate the payload: the tab is opened by
// replaying a click, not by navigating here.
bool IsOpenableLinkDrag(IDataObject* data_object) {
  std::wstring url;
  return ReadDraggedUrl(data_object, url) && IsAllowedScheme(url);
}

// Hides the URI formats from the real drop target. Swallowing Drop instead
// would leave the renderer without its DragSourceEndedAt and wedge the drag
// session: later mouse events stay drag events and text selection sticks.
class SanitizedDataObject final : public IDataObject {
 public:
  explicit SanitizedDataObject(IDataObject* inner) : inner_(inner) {
    if (inner_) {
      inner_->AddRef();
    }
  }

  [[nodiscard]] bool valid() const { return inner_ != nullptr; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void** object) override {
    if (!object) {
      return E_POINTER;
    }
    if (iid == IID_IUnknown || iid == IID_IDataObject) {
      *object = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --refs_;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format,
                                    STGMEDIUM* medium) override {
    if (format && IsStrippable(*format)) {
      return DV_E_FORMATETC;
    }
    return inner_->GetData(format, medium);
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* format,
                                         STGMEDIUM* medium) override {
    if (format && IsStrippable(*format)) {
      return DV_E_FORMATETC;
    }
    return inner_->GetDataHere(format, medium);
  }

  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (format && IsStrippable(*format)) {
      return DV_E_FORMATETC;
    }
    return inner_->QueryGetData(format);
  }

  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
      FORMATETC* format, FORMATETC* canonical) override {
    if (format && IsStrippable(*format)) {
      return DV_E_FORMATETC;
    }
    return inner_->GetCanonicalFormatEtc(format, canonical);
  }

  HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium,
                                    BOOL release) override {
    return inner_->SetData(format, medium, release);
  }

  // The full list is advertised on purpose: Blink re-queries per format through
  // QueryGetData/GetData, which report the stripped ones as unavailable.
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(
      DWORD direction, IEnumFORMATETC** enumerator) override {
    return inner_->EnumFormatEtc(direction, enumerator);
  }

  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* format, DWORD advise_flags,
                                    IAdviseSink* advise_sink,
                                    DWORD* connection) override {
    return inner_->DAdvise(format, advise_flags, advise_sink, connection);
  }

  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD connection) override {
    return inner_->DUnadvise(connection);
  }

  HRESULT STDMETHODCALLTYPE EnumDAdvise(
      IEnumSTATDATA** advise_enumerator) override {
    return inner_->EnumDAdvise(advise_enumerator);
  }

 private:
  [[nodiscard]] static bool IsStrippable(const FORMATETC& format) {
    switch (format.cfFormat) {
      case CF_UNICODETEXT:
      case CF_TEXT:
        return true;
      default:
        break;
    }
    if (!IsRegisteredFormat(format.cfFormat)) {
      return false;
    }
    wchar_t name[64]{};
    const int length = GetClipboardFormatNameW(
        format.cfFormat, name, static_cast<int>(ARRAYSIZE(name)));
    if (length <= 0) {
      return false;
    }
    const std::wstring_view view(name, static_cast<size_t>(length));
    return view == L"text/uri-list" || view == L"text/plain";
  }

  IDataObject* inner_ = nullptr;
  ULONG refs_ = 1;
};

// Drop runs inside OLE's modal drag loop, so acting there would re-enter
// Chrome's UI thread mid-drag. Mark the drop and act once the loop unwinds.
bool pending_drop = false;
HWND pending_hwnd = nullptr;

constexpr UINT_PTR kOpenTimerId = 0x64724470;  // 'ddDp'

// Replays a Ctrl+click on the link: Chrome's own gesture for opening it in a
// background tab next to the current one, so placement, activation and focus
// all stay Chrome's business. Alt+Enter cannot be used for this because the
// omnibox always appends at the end of the strip.
void OpenInBackgroundTab(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) {
    return;
  }

  // The drag began on the link and the mouse hook recorded that press.
  const POINT start = GetDragStartPoint();
  if (start.x < 0 || start.y < 0) {
    return;
  }

  // Dropping into another window or the desktop must not turn into a click on
  // whatever now sits under the old press position.
  POINT current{};
  if (!::GetCursorPos(&current)) {
    return;
  }
  const HWND point_window = ::WindowFromPoint(current);
  if ((point_window ? ::GetAncestor(point_window, GA_ROOT) : nullptr) != hwnd) {
    return;
  }

  CtrlClickAt(start);
}

bool OpenPendingDrop() {
  if (!pending_drop) {
    return false;
  }

  const HWND hwnd = pending_hwnd;
  pending_drop = false;
  pending_hwnd = nullptr;

  if (!IsChromeWindow(hwnd)) {
    return false;
  }
  OpenInBackgroundTab(hwnd);
  return true;
}

void CALLBACK OpenTimerProc(HWND hwnd, UINT, UINT_PTR event_id, DWORD) {
  KillTimer(hwnd, event_id);
  OpenPendingDrop();
}

// One rect resolve per call; walking the UIA tree per point is too slow here.
bool IsOverTabStrip(POINT pt) {
  const HWND point_window = WindowFromPoint(pt);
  const HWND root = point_window ? GetAncestor(point_window, GA_ROOT) : nullptr;
  const auto rect = GetTabStripRect(root);
  return rect && PtInRect(&*rect, pt) != FALSE;
}

class DropTargetProxy final : public IDropTarget {
 public:
  explicit DropTargetProxy(IDropTarget* inner) : inner_(inner) {
    if (inner_) {
      inner_->AddRef();
    }
  }

  [[nodiscard]] bool valid() const { return inner_ != nullptr; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void** object) override {
    if (!object) {
      return E_POINTER;
    }
    if (iid == IID_IUnknown || iid == IID_IDropTarget) {
      *object = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --refs_;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  // Chrome gates same-tab link drops behind SupportOpeningDraggedLinksInSameTab,
  // so its target answers DROPEFFECT_NONE over the content and OLE then never
  // calls Drop. Claim the drag here instead, while the data object is readable.
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data_object,
                                      DWORD key_state, POINTL pt,
                                      DWORD* effect) override {
    const HRESULT result = inner_->DragEnter(data_object, key_state, pt, effect);

    claimed_ = false;
    if (!config.IsDragLinkNewTab() || !data_object || effect == nullptr) {
      return result;
    }

    // The SDK has no DROPEFFECT_ALL; mask the effect bits explicitly.
    constexpr DWORD kEffectMask = DROPEFFECT_COPY | DROPEFFECT_MOVE |
                                  DROPEFFECT_LINK | DROPEFFECT_SCROLL |
                                  DROPEFFECT_NONE;
    if ((*effect & kEffectMask) != DROPEFFECT_NONE) {
      // Chrome accepts it somewhere (tab strip, bookmark bar, a page that
      // handles the drop).
      return result;
    }
    if (!IsLinkDrag(data_object) || !IsOpenableLinkDrag(data_object)) {
      return result;
    }

    // Same exclusion list as the other new-tab enhancements: when the current
    // tab is excluded, do not claim the drag at all, so the drop falls back to
    // Chrome's own handling.
    const HWND point_window = ::WindowFromPoint(POINT{pt.x, pt.y});
    const HWND root =
        point_window ? ::GetAncestor(point_window, GA_ROOT) : nullptr;
    if (config.IsNewTabDisable() &&
        IsOnNewTab(root, config.GetDisableTabNames())) {
      return result;
    }

    claimed_ = true;
    *effect = DROPEFFECT_LINK;
    return result;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL pt,
                                     DWORD* effect) override {
    const HRESULT result = inner_->DragOver(key_state, pt, effect);
    if (claimed_ && effect != nullptr) {
      // Chrome keeps answering NONE over the content; keep the claim alive.
      *effect = DROPEFFECT_LINK;
    }
    return result;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    claimed_ = false;
    return inner_->DragLeave();
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data_object, DWORD key_state,
                                 POINTL pt, DWORD* effect) override {
    auto forward = [&] {
      pending_drop = false;
      pending_hwnd = nullptr;
      claimed_ = false;
      return inner_->Drop(data_object, key_state, pt, effect);
    };

    if (!claimed_ || !data_object) {
      return forward();
    }

    // The drag may have wandered onto the tab strip or bookmark bar since
    // DragEnter, where Chrome's own handling applies.
    const POINT screen_pt{pt.x, pt.y};
    if (IsOverTabStrip(screen_pt) || IsOnBookmarkBarZone(screen_pt)) {
      return forward();
    }

    const HWND point_window = WindowFromPoint(screen_pt);
    const HWND window =
        point_window ? GetAncestor(point_window, GA_ROOT) : nullptr;
    if (!IsChromeWindow(window)) {
      return forward();
    }

    SanitizedDataObject sanitized(data_object);
    if (!sanitized.valid()) {
      return forward();
    }

    pending_drop = true;
    pending_hwnd = window;
    // Backstop for a pointer that stays still after the drop.
    ::SetTimer(window, kOpenTimerId, 250, OpenTimerProc);

    const HRESULT result = inner_->Drop(&sanitized, key_state, pt, effect);
    // Report the drop as unhandled so the source does not treat the page as a
    // link target.
    if (effect) {
      *effect = DROPEFFECT_NONE;
    }
    return result;
  }

 private:
  IDropTarget* inner_ = nullptr;
  ULONG refs_ = 1;
  // Set when DragEnter claimed the drag; Drop only takes over while it holds.
  bool claimed_ = false;
};

HRESULT WINAPI MyRegisterDragDrop(HWND hwnd, IDropTarget* target) {
  if (!target || !IsChromeWindow(hwnd)) {
    return RawRegisterDragDrop(hwnd, target);
  }

  auto* proxy = new DropTargetProxy(target);
  if (!proxy->valid()) {
    delete proxy;
    return RawRegisterDragDrop(hwnd, target);
  }

  const HRESULT result = RawRegisterDragDrop(hwnd, proxy);
  if (FAILED(result)) {
    proxy->Release();
    return result;
  }
  return result;
}

}  // namespace

// One SendInput batch so Chrome sees a normal gesture; the magic code keeps our
// own low-level mouse hook from reacting to it.
void CtrlClickAt(POINT pt) {
  // MOUSEEVENTF_ABSOLUTE wants virtual-desktop coordinates scaled to 0..65535.
  const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 1 || height <= 1) {
    return;
  }
  const int origin_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int origin_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);

  INPUT inputs[5]{};
  int count = 0;

  inputs[count].type = INPUT_KEYBOARD;
  inputs[count].ki.wVk = VK_CONTROL;
  inputs[count].ki.dwExtraInfo = GetMagicCode();
  ++count;

  inputs[count].type = INPUT_MOUSE;
  inputs[count].mi.dx = ::MulDiv(pt.x - origin_x, 65535, width - 1);
  inputs[count].mi.dy = ::MulDiv(pt.y - origin_y, 65535, height - 1);
  inputs[count].mi.dwFlags =
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  inputs[count].mi.dwExtraInfo = GetMagicCode();
  ++count;

  inputs[count].type = INPUT_MOUSE;
  inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  inputs[count].mi.dwExtraInfo = GetMagicCode();
  ++count;

  inputs[count] = inputs[count - 1];
  inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  ++count;

  inputs[count].type = INPUT_KEYBOARD;
  inputs[count].ki.wVk = VK_CONTROL;
  inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
  inputs[count].ki.dwExtraInfo = GetMagicCode();
  ++count;

  ::SendInput(static_cast<UINT>(count), inputs, sizeof(INPUT));
}

bool FlushPendingDragDrop() {
  return OpenPendingDrop();
}

void DragLinkNewTab() {
  // Installed unconditionally: Chrome registers its drop target at startup,
  // long before the config page can be opened, so gating on the current value
  // would make a runtime toggle a no-op until the next launch. DragEnter
  // re-checks the config, so an installed proxy costs one forward call.
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<LPVOID*>(&RawRegisterDragDrop),
               reinterpret_cast<void*>(MyRegisterDragDrop));
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"Hook RegisterDragDrop failed {}", status);
  }
}
