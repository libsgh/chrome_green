#include "appid.h"

#include <propidl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "detours.h"
#include "sqlite_hist.h"
#include "update.h"
#include "utils.h"
#include "config.h"

// PKEY_Title (System.Title) — defined in propkey.h, but pulling that header in
// trips clang-cl 19 on Windows SDK 10.0.26100.0: propkeydef.h's
// `operator==(REFPROPERTYKEY)` references PROPERTYKEY before it is visible.
// Declaring the key inline (the documented GUID/PID) avoids the header entirely.
//   System.Title = {F29F85E0-4FF9-1068-AB91-08002B27B3D9}, PID 2
static const PROPERTYKEY PKEY_Title = {
    {0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}},
    2};

// System.AppUserModel.ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, PID 5
static const PROPERTYKEY PKEY_AppUserModel_ID = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
    5};

namespace {

bool KeyEquals(REFPROPERTYKEY a, const PROPERTYKEY& b) {
  return a.fmtid == b.fmtid && a.pid == b.pid;
}

// Split the process command line into arguments the way Chromium's own
// CommandLine parser does: a '"' toggles quoting anywhere inside a token and
// is dropped; whitespace outside quotes separates arguments.
//
// This matters because portable.cc quotes a WHOLE switch when its value
// contains a space (QuoteSpaceIfNeeded in utils.cc), so the line contains
//   "--user-data-dir=D:\Program Files\Chrome\Data"
// A naive `wcsstr("--user-data-dir=")` scan then stops at the space and yields
// "D:\Program" — which made the History / Favicons / Local State lookups point
// into a non-existent directory whenever Chrome lives under a path with a
// space (e.g. "Program Files"), silently emptying the Recent category.
std::vector<std::wstring> TokenizeCommandLine() {
  std::vector<std::wstring> args;
  const wchar_t* p = GetCommandLineW();
  if (!p) return args;
  std::wstring cur;
  bool in_quotes = false;
  bool have_cur = false;
  for (; *p; ++p) {
    const wchar_t c = *p;
    if (c == L'"') {
      in_quotes = !in_quotes;
      have_cur = true;
      continue;
    }
    if (!in_quotes && (c == L' ' || c == L'\t')) {
      if (have_cur) {
        args.push_back(cur);
        cur.clear();
        have_cur = false;
      }
      continue;
    }
    cur += c;
    have_cur = true;
  }
  if (have_cur) args.push_back(cur);
  return args;
}

// Value of a `--key=value` switch (`key` must include the trailing '='), or "".
std::wstring GetCmdArgValue(const wchar_t* key) {
  const std::wstring k(key);
  for (const std::wstring& a : TokenizeCommandLine()) {
    if (a.size() > k.size() && a.compare(0, k.size(), k) == 0)
      return a.substr(k.size());
  }
  return L"";
}

bool FileExists(const wchar_t* path) {
  if (!path || !*path) return false;
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Chrome's own jump-list task labels, per locale. These are stable UI strings
// that rarely change, so hardcoding them (rather than parsing Chrome's locale
// pak, whose GRIT resource ids shift every version) keeps our Tasks visually
// indistinguishable from a native Chrome jump list — including correct casing
// ("New window", not "New Window") and the right language for the active UI.
struct LocaleLabels {
  const char* tag;  // "en", "zh-CN", ...
  const wchar_t* new_window;
  const wchar_t* new_incognito;
  const wchar_t* recent;  // jump-list "Recent" category header
};

static const LocaleLabels kLocaleLabels[] = {
    {"en", L"New window", L"New Incognito window", L"Recent"},
    {"en-US", L"New window", L"New Incognito window", L"Recent"},
    {"en-GB", L"New window", L"New Incognito window", L"Recent"},
    // Chrome's jump-list task labels are the strings it ACTUALLY displays in the
    // taskbar menu.  AUTHORITATIVE SOURCE: extracted directly from the installed
    // Chrome's Locales/<lang>.pak (UTF-8, version-5 data pack; ids match
    // resources.pak / en-US.pak).  Native Chrome STRIPS the '&' shortcut marker
    // (ReplaceSubstringsAfterOffset) before assigning the title, so a resource
    // "打开新的无痕式窗口(&I)" shows as "打开新的无痕式窗口(I)" with NO working
    // accelerator — the (I)/(N) are purely cosmetic, matching native Chrome.
    // We therefore keep the labels WITHOUT '&' (matching native display AND
    // native behavior: jump-list Tasks have no functional N/I shortcuts).  The
    // CJK literal "(I)"/"(N)" are kept as-is since that is exactly what native
    // shows after the strip.  We follow the shipped Chrome build, NOT upstream
    // Chromium's .grd/.desktop files (they differ for some languages).
    {"zh-CN", L"打开新的窗口(N)", L"打开新的无痕式窗口(I)", L"最近打开的标签页"},
    {"zh-TW", L"開啟新視窗", L"新增無痕視窗(I)", L"最近開啟的標籤頁"},
    {"zh-HK", L"開啟新視窗", L"新增無痕視窗(I)", L"最近開啟的標籤頁"},
    {"ja", L"新しいウィンドウ", L"新しいシークレット ウィンドウ(I)", L"最近のページ"},
    {"ko", L"새 창", L"새 시크릿 창(I)", L"최근 탭"},
    {"es", L"Nueva ventana", L"Nueva ventana de incógnito", L"Recientes"},
    {"es-419", L"Nueva ventana", L"Nueva ventana de incógnito", L"Recientes"},
    {"fr", L"Nouvelle fenêtre", L"Nouvelle fenêtre de navigation privée", L"Récents"},
    {"fr-CA", L"Nouvelle fenêtre", L"Nouvelle fenêtre de navigation privée", L"Récents"},
    {"de", L"Neues Fenster", L"Neues Inkognitofenster", L"Zuletzt geschlossen"},
    {"ru", L"Новое окно", L"Новое окно в режиме инкогнито", L"Недавние"},
    {"pt-BR", L"Nova janela", L"Nova janela anônima", L"Recentes"},
    {"pt-PT", L"Nova janela", L"Nova janela de navegação anónima", L"Recentes"},
    {"it", L"Nuova finestra", L"Nuova finestra di navigazione in incognito", L"Recenti"},
    {"nl", L"Nieuw venster", L"Nieuw incognitovenster", L"Recent"},
    {"ar", L"نافذة جديدة", L"نافذة جديدة للتصفُّح المتخفي", L"الأخيرة"},
    {"tr", L"Yeni pencere", L"Yeni Gizli pencere", L"Son açılanlar"},
    {"pl", L"Nowe okno", L"Nowe okno incognito", L"Ostatnie"},
    {"th", L"หน้าต่างใหม่", L"หน้าต่างใหม่ที่ไม่ระบุตัวตน", L"ล่าสุด"},
    {"vi", L"Cửa sổ mới", L"Cửa sổ ẩn danh mới", L"Gần đây"},
    {"id", L"Jendela baru", L"Jendela Samaran baru", L"Terbaru"},
    {"hi", L"नई विंडो", L"नई गुप्त विंडो", L"हाल ही में"},
};

// Case-insensitive ASCII compare of a wide string (locale) against a narrow
// tag. Char-by-char; no locale-aware collation needed for ASCII tags.
bool CiEquals(const wchar_t* a, const char* b) {
  while (*a && *b) {
    wchar_t ca = (*a >= L'A' && *a <= L'Z') ? *a + (L'a' - L'A') : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + ('a' - 'A') : *b;
    if (ca != static_cast<wchar_t>(cb)) return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

// Pick the (new_window, new_incognito) labels for `locale`. Exact region tags
// (e.g. zh-CN) win; otherwise the language subtag (e.g. "ja") is matched;
// unknown locales fall back to English.
void PickLabels(const std::wstring& locale, const wchar_t*& nw,
                const wchar_t*& ni, const wchar_t*& recent) {
  for (const auto& e : kLocaleLabels) {
    bool has_region = std::strchr(e.tag, '-') != nullptr;
    if (has_region) {
      if (CiEquals(locale.c_str(), e.tag)) {
        nw = e.new_window;
        ni = e.new_incognito;
        recent = e.recent;
        return;
      }
    } else {
      std::wstring lang = locale;
      const size_t dash = lang.find(L'-');
      if (dash != std::wstring::npos) lang = lang.substr(0, dash);
      if (CiEquals(lang.c_str(), e.tag)) {
        nw = e.new_window;
        ni = e.new_incognito;
        recent = e.recent;
        return;
      }
    }
  }
  // fallback: English (already the defaults passed in)
}

// Read a whitespace/quote-delimited token starting at q, lowercased (ASCII).
std::wstring ReadLangToken(const wchar_t* q) {
  while (*q == L' ' || *q == L'\t') q++;
  const wchar_t* start = q;
  while (*q && *q != L' ' && *q != L'\t' && *q != L'"' && *q != L'\'' &&
         *q != L'>' && *q != L'<')
    q++;
  std::wstring out(start, q);
  for (wchar_t& c : out) {
    if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
  }
  return out;
}

// Determine Chrome's active UI locale: --lang takes priority (what the user
// explicitly set), otherwise the system UI language. Returns e.g. "en-US",
// "zh-cn".
// Chrome persists its active UI locale in <user-data-dir>/Local State under
// the "intl" dict ("app_locale").  chrome://settings/languages writes there
// rather than as a --lang flag, so to match native Chrome's jump-list language
// we read it directly.  Returns e.g. "en-us" or "" on any failure.
static std::wstring ReadChromeLocaleFromLocalState() {
  // Resolve user-data-dir. Portable mode injects --user-data-dir into the
  // command line, which GetCommandLineW() returns with the hook applied. The
  // switch is quoted as a whole when its value contains a space, so it must be
  // parsed with the same tokenizer (see TokenizeCommandLine).
  std::wstring ud = GetCmdArgValue(L"--user-data-dir=");
  if (ud.empty()) {
    wchar_t la[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n && n < MAX_PATH)
      ud = std::wstring(la) + L"\\Google\\Chrome\\User Data";
    else
      return L"";
  }
  if (ud.empty()) return L"";
  std::wstring path = ud + L"\\Local State";
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return L"";
  LARGE_INTEGER fsz;
  if (!GetFileSizeEx(h, &fsz) || fsz.QuadPart > 32 * 1024 * 1024) {
    CloseHandle(h);
    return L"";
  }
  std::string content;
  content.reserve((size_t)fsz.QuadPart);
  char buf[1 << 16];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
    content.append(buf, got);
  CloseHandle(h);
  // "intl":{"app_locale":"xx-YY",...}  ->  match the inner key.
  static const char kKey[] = "\"app_locale\":\"";
  size_t pos = content.find(kKey);
  if (pos == std::string::npos) return L"";
  pos += sizeof(kKey) - 1;
  size_t end = content.find('"', pos);
  if (end == std::string::npos || end == pos) return L"";
  std::string loc = content.substr(pos, end - pos);
  std::wstring wloc;
  wloc.reserve(loc.size());
  for (char c : loc) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    wloc += (wchar_t)(unsigned char)c;
  }
  return wloc;
}

std::wstring GetChromeLocale() {
  const wchar_t* cl = GetCommandLineW();
  for (const wchar_t* p = cl; *p; ++p) {
    if (p[0] == L'-' && p[1] == L'-' && (p[2] == L'l' || p[2] == L'L') &&
        (p[3] == L'a' || p[3] == L'A') && (p[4] == L'n' || p[4] == L'N') &&
        (p[5] == L'g' || p[5] == L'G')) {
      const wchar_t* q = p + 6;
      if (*q == L'=') return ReadLangToken(q + 1);
      if (*q == L' ' || *q == L'\t') return ReadLangToken(q);
    }
  }
  // No --lang: use Chrome's own UI locale from its Local State file
  // (chrome://settings/languages) so our menu tracks the browser language.
  std::wstring ls = ReadChromeLocaleFromLocalState();
  if (!ls.empty()) return ls;
  // Fall back to the Windows system UI language.
  LANGID lid = GetUserDefaultUILanguage();
  wchar_t name[96] = {0};
  if (LCIDToLocaleName(MAKELCID(lid, SORT_DEFAULT), name, 96, 0) &&
      name[0]) {
    for (wchar_t* c = name; *c; ++c) {
      if (*c >= L'A' && *c <= L'Z') *c = *c - L'A' + L'a';
    }
    return std::wstring(name);
  }
  return L"en-us";
}

}  // namespace

// ---------------------------------------------------------------------------
// Window-level AppUserModelID forcing.
//
// Chrome sets its own native AUMID per window via SHGetPropertyStoreForWindow.
// We intercept that write and replace it with our stable per-install id
// (g_our_aumid, see SetAppId), so process / windows / pinned .lnk share one
// identity: a single taskbar icon and no Chrome-native jump list.
// Tasks are still registered at runtime because Chrome's own point at a
// possibly-wrong install path (version.dll hijacking).
// ---------------------------------------------------------------------------

// Chrome's native AUMID, diagnostics only (registration uses g_our_aumid).
static std::wstring g_captured_aumid;
static CRITICAL_SECTION g_aumid_cs;
static bool g_aumid_cs_inited = false;

// Resolved AUMID (final) and the History DB path, set once Chrome's identity
// is known. Used by the periodic jump-list refresh thread so it can re-register
// the "Recent" category from Chrome's History without re-capturing the AUMID.
static std::wstring g_resolved_aumid;
static std::wstring g_history_path;
static std::wstring g_favicons_path;  // Chrome's Favicons DB (same dir as History)

// Our stable per-install AUMID. Resolved once at startup (see SetAppId) from
// the AUMID the UPDATER stamped onto chrome.exe — so appid.cc and the updater
// agree on the EXACT same string, which the taskbar-pinned .lnk also carries.
//
// We force the PROCESS, every Chrome WINDOW (via the SHGetPropertyStoreForWindow
// wrapper), and the pinned .lnk onto THIS id. Consequences:
//   * Single taskbar icon — process, windows and .lnk all share one identity.
//   * Before Chrome launches the .lnk points at an id under which NOTHING is
//     registered yet (the updater deliberately does not create a jump list), so
//     right-clicking it shows only the system "Unpin" item — no English menu.
//   * After launch we register our localized Tasks + Recent under this id, so
//     the menu appears (Chinese) only once the browser is running.
static std::wstring g_our_aumid;

// ---------------------------------------------------------------------------
// Recent-items capture via SHAddToRecentDocs hook.
// Chrome calls SHAddToRecentDocs with an IShellLink for each tab it wants to
// appear in the "Recent" section of its jump list.  We intercept those calls,
// extract the URL + title, and store them so we can re-register them under our
// own AUMID (so they appear on the correct taskbar button).
// ---------------------------------------------------------------------------

struct RecentItem {
  std::wstring url;
  std::wstring title;
};

// Ring buffer of recently captured items (max 10).  Newer items at lower
// indices; we keep them in reverse chronological order for display.
static const int kMaxRecentItems = 10;
static RecentItem g_recent_items[kMaxRecentItems];
static int g_recent_count = 0;
static CRITICAL_SECTION g_recent_cs;
static bool g_recent_cs_inited = false;

// Serializes RegisterTasks so the initial (delayed) call and the live
// "recent items arrived" re-registration don't run BeginList/CommitList on the
// same AUMID concurrently.
static CRITICAL_SECTION g_reg_cs;
static bool g_reg_cs_inited = false;
static void RegCsInit() {
  if (!g_reg_cs_inited) {
    InitializeCriticalSection(&g_reg_cs);
    g_reg_cs_inited = true;
  }
}

// Forward declaration — used by the live re-registration worker thread below,
// defined later in this file.
static void RegisterTasks(const std::wstring& aumid);

// Forward declaration — clears all Tasks/Recent categories for an AUMID so the
// taskbar menu only shows "Unpin". Used by SetAppId (off branch) and the refresh
// thread below; defined later in this file.
static void ClearJumpList(const std::wstring& aumid);

// Forward declaration — defined later in this file (reads Chrome's History DB
// for the "Recent" jump-list category).
static void ReadRecentEntries(std::vector<sqlite_hist::Entry>& out);

// Forward declaration — defined later in this file (writes a PNG blob as a
// single-image .ico file for jump-list favicon icons).
static std::wstring WriteFaviconIco(const std::vector<uint8_t>& png_data,
                                    int index);

static void RecentCsInit() {
  if (!g_recent_cs_inited) {
    InitializeCriticalSection(&g_recent_cs);
    g_recent_cs_inited = true;
  }
}

// Push a new item into the ring buffer (thread-safe).
static void PushRecentItem(const std::wstring& url, const std::wstring& title) {
  RecentCsInit();
  EnterCriticalSection(&g_recent_cs);
  // Shift existing items right (drop oldest if full).
  if (g_recent_count >= kMaxRecentItems)
    g_recent_count = kMaxRecentItems - 1;
  for (int i = g_recent_count; i > 0; --i)
    g_recent_items[i] = g_recent_items[i - 1];
  g_recent_items[0].url = url;
  g_recent_items[0].title = title.empty() ? url : title;
  if (g_recent_count < kMaxRecentItems) g_recent_count++;
  LeaveCriticalSection(&g_recent_cs);
}

// Try to extract URL and title from an IShellLink that Chrome passed to
// SHAddToRecentDocs.
static void ExtractFromShellLink(IShellLinkW* sl, std::wstring* out_url,
                                  std::wstring* out_title) {
  // GetArguments often contains the URL for web shortcuts.
  wchar_t args[MAX_PATH] = {0};
  if (SUCCEEDED(sl->GetPath(args, MAX_PATH, nullptr, 0)) && args[0]) {
    *out_url = args;
  }
  // Get arguments (Chrome may pass URL here instead).
  wchar_t argbuf[2048] = {0};
  if (SUCCEEDED(sl->GetArguments(argbuf, 2048)) && argbuf[0]) {
    // If we already have a path and it looks like chrome.exe, prefer args
    // as the URL (that's how Chrome encodes tab URLs).
    std::wstring arg(argbuf);
    if (arg.find(L"http://") == 0 || arg.find(L"https://") == 0 ||
        arg.find(L"chrome://") == 0 || arg.find(L"about:") == 0) {
      *out_url = arg;
    }
  }
  // Get title from PKEY_Title on the link's property store.
  IPropertyStore* ps = nullptr;
  if (SUCCEEDED(sl->QueryInterface(IID_IPropertyStore, (void**)&ps)) && ps) {
    PROPVARIANT pv;
    if (SUCCEEDED(ps->GetValue(PKEY_Title, &pv)) && pv.vt == VT_LPWSTR &&
        pv.pwszVal && pv.pwszVal[0])
      *out_title = pv.pwszVal;
    PropVariantClear(&pv);
    ps->Release();
  }
}

static void AumidCsInit() {
  if (!g_aumid_cs_inited) {
    InitializeCriticalSection(&g_aumid_cs);
    g_aumid_cs_inited = true;
  }
}
static void SetCapturedAumid(const wchar_t* id) {
  if (!id || !*id) return;
  AumidCsInit();
  EnterCriticalSection(&g_aumid_cs);
  g_captured_aumid = id;
  LeaveCriticalSection(&g_aumid_cs);
}

class PropertyStoreWrapper : public IPropertyStore {
 public:
  PropertyStoreWrapper(IPropertyStore* inner)
      : m_inner(inner), m_ref(1) {
    if (m_inner) m_inner->AddRef();
  }
  virtual ~PropertyStoreWrapper() {
    if (m_inner) m_inner->Release();
  }

  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IPropertyStore || riid == IID_IUnknown) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return m_inner ? m_inner->QueryInterface(riid, ppv) : E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  IFACEMETHODIMP_(ULONG) Release() override {
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
  }

  IFACEMETHODIMP GetCount(DWORD* c) override { return m_inner->GetCount(c); }
  IFACEMETHODIMP GetAt(DWORD i, PROPERTYKEY* p) override {
    return m_inner->GetAt(i, p);
  }
  IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
    return m_inner->GetValue(key, pv);
  }
  IFACEMETHODIMP SetValue(REFPROPERTYKEY key, const PROPVARIANT& pv) override {
    // Force every window onto our AUMID so windows, process and pinned .lnk
    // group as one icon instead of falling back to Chrome's native identity.
    if (KeyEquals(key, PKEY_AppUserModel_ID) && !g_our_aumid.empty()) {
      PROPVARIANT v = {};
      v.vt = VT_LPWSTR;
      v.pwszVal = const_cast<wchar_t*>(g_our_aumid.c_str());
      // Do NOT PropVariantClear — pwszVal points at our string (not CoTaskMem).
      return m_inner->SetValue(key, v);
    }
    return m_inner->SetValue(key, pv);
  }
  IFACEMETHODIMP Commit() override { return m_inner->Commit(); }

 private:
  IPropertyStore* m_inner;
  ULONG m_ref;
};

static decltype(&SHGetPropertyStoreForWindow) RealSHGetPropertyStoreForWindow =
    nullptr;

HRESULT WINAPI MySHGetPropertyStoreForWindow(HWND hwnd, REFIID riid,
                                             void** ppv) {
  HRESULT hr = RealSHGetPropertyStoreForWindow(hwnd, riid, ppv);
  if (SUCCEEDED(hr) && ppv && *ppv && riid == IID_IPropertyStore) {
    IPropertyStore* inner = static_cast<IPropertyStore*>(*ppv);
    IPropertyStore* wrapped = new PropertyStoreWrapper(inner);
    inner->Release();  // wrapper holds its own ref
    *ppv = wrapped;
  }
  return hr;
}

// Chrome sets its process AUMID here; we pass it through and override it with
// our own in the SetAppId worker thread.
static HRESULT(WINAPI* RealSetCurrentProcessExplicitAppUserModelID)(PCWSTR) =
    nullptr;

HRESULT WINAPI MySetCurrentProcessExplicitAppUserModelID(PCWSTR AppID) {
  if (AppID && *AppID) SetCapturedAumid(AppID);
  return RealSetCurrentProcessExplicitAppUserModelID
             ? RealSetCurrentProcessExplicitAppUserModelID(AppID)
             : E_FAIL;
}

static void InstallAumidHooks() {
  if (RealSHGetPropertyStoreForWindow &&
      RealSetCurrentProcessExplicitAppUserModelID)
    return;  // idempotent
  AumidCsInit();
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  if (!RealSHGetPropertyStoreForWindow) {
    RealSHGetPropertyStoreForWindow = &SHGetPropertyStoreForWindow;
    DetourAttach(reinterpret_cast<PVOID*>(&RealSHGetPropertyStoreForWindow),
                 reinterpret_cast<PVOID>(MySHGetPropertyStoreForWindow));
  }
  if (!RealSetCurrentProcessExplicitAppUserModelID) {
    RealSetCurrentProcessExplicitAppUserModelID =
        &SetCurrentProcessExplicitAppUserModelID;
    DetourAttach(
        reinterpret_cast<PVOID*>(&RealSetCurrentProcessExplicitAppUserModelID),
        reinterpret_cast<PVOID>(MySetCurrentProcessExplicitAppUserModelID));
  }
  DetourTransactionCommit();
}

// ---------------------------------------------------------------------------
// SHAddToRecentDocs hook — capture items Chrome adds to its "Recent" jump-list
// section so we can re-register them under our AUMID.
//
// IMPORTANT: Chrome's OWN "recently closed" jump-list items are normally built
// with ICustomDestinationList directly (NOT delivered through
// SHAddToRecentDocs). This hook is a best-effort capture: it handles the
// SHARD_LINK / SHARD_PIDL / SHARD_PATH forms, and re-registers the jump list
// LIVE whenever a new item arrives (debounced) so the Recent category stays
// current during a session instead of only at startup.
// ---------------------------------------------------------------------------

static decltype(&SHAddToRecentDocs) RealSHAddToRecentDocs = nullptr;

// Signalled when a recent item is captured; the worker thread waits on it.
static HANDLE g_recent_evt = nullptr;

static bool IsWebUrl(const std::wstring& s) {
  return s.compare(0, 7, L"http://") == 0 ||
         s.compare(0, 8, L"https://") == 0 ||
         s.compare(0, 9, L"chrome://") == 0 ||
         s.compare(0, 6, L"about:") == 0 ||
         s.compare(0, 7, L"file://") == 0;
}

// Parse a .url (Internet shortcut) file for its Target URL.
static bool ReadUrlFile(const std::wstring& path, std::wstring* out_url) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const DWORD kSize = 65536;
  char* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, kSize));
  DWORD rd = 0;
  ReadFile(h, buf, kSize - 1, &rd, nullptr);
  CloseHandle(h);
  if (!rd) {
    HeapFree(GetProcessHeap(), 0, buf);
    return false;
  }
  buf[rd] = 0;
  int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(rd),
                                 nullptr, 0);
  std::wstring w;
  if (wlen > 0) {
    w.resize(wlen);
    MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(rd), &w[0], wlen);
  }
  HeapFree(GetProcessHeap(), 0, buf);
  bool in_section = false;
  size_t i = 0;
  while (i < w.size()) {
    size_t nl = w.find(L'\n', i);
    if (nl == std::wstring::npos) nl = w.size();
    std::wstring line = w.substr(i, nl - i);
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    if (!line.empty() && line[0] == L'[') {
      in_section = (line.find(L"[InternetShortcut]") != std::wstring::npos);
    } else if (in_section) {
      if (line.compare(0, 4, L"URL=") == 0) {
        *out_url = line.substr(4);
        return true;
      }
      if (line.compare(0, 6, L"URL =") == 0) {
        *out_url = line.substr(6);
        return true;
      }
    }
    i = nl + 1;
  }
  return false;
}

// Extract URL + title from whatever Chrome passed to SHAddToRecentDocs.
static void CaptureRecentFromFlags(UINT uFlags, LPCVOID pv) {
  std::wstring url, title;
  if (!pv) return;

  if (uFlags == SHARD_LINK) {
    IUnknown* unk = const_cast<IUnknown*>(static_cast<const IUnknown*>(pv));
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_IShellLinkW, (void**)&sl)) && sl) {
      ExtractFromShellLink(sl, &url, &title);
      sl->Release();
    }
  } else if (uFlags == SHARD_PIDL) {
    PCIDLIST_ABSOLUTE pidl = reinterpret_cast<PCIDLIST_ABSOLUTE>(pv);
    PWSTR name = nullptr;
    // Only treat genuine web URLs as tabs (avoid mis-detecting a .url file's
    // file:// URL as a tab).
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_URL, &name)) && name) {
      std::wstring u(name);
      CoTaskMemFree(name);
      if (u.compare(0, 7, L"http://") == 0 || u.compare(0, 8, L"https://") == 0 ||
          u.compare(0, 9, L"chrome://") == 0 || u.compare(0, 6, L"about:") == 0) {
        url = u;
        PWSTR disp = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY,
                                          &disp)) && disp) {
          title = disp;
          CoTaskMemFree(disp);
        }
      }
    }
    if (url.empty()) {
      // Not a web URL — maybe a .url file pidl. Resolve its parsing path and
      // read the target URL from the shortcut file.
      if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING,
                                        &name)) && name) {
        std::wstring p(name);
        CoTaskMemFree(name);
        if (p.size() > 4 && (p.compare(p.size() - 4, 4, L".url") == 0 ||
                             p.compare(p.size() - 4, 4, L".URL") == 0))
          ReadUrlFile(p, &url);
      }
    }
  } else if (uFlags == SHARD_PATH) {
    std::wstring p = static_cast<LPCWSTR>(pv);
    if (p.size() > 4 && (p.compare(p.size() - 4, 4, L".url") == 0 ||
                         p.compare(p.size() - 4, 4, L".URL") == 0))
      ReadUrlFile(p, &url);
    else if (IsWebUrl(p))
      url = p;
  } else {
    AddDebugLog("appid: RecentDocs unsupported uFlags=" +
                std::to_string(uFlags));
    return;
  }

  if (!IsWebUrl(url)) return;  // ignore non-tab items (exe, files, etc.)
  PushRecentItem(url, title);
  AddDebugLog("appid: RecentDocs captured uFlags=" + std::to_string(uFlags) +
              " url=" + WstrToUtf8(url));
  // Signal live re-registration.
  if (g_recent_evt) SetEvent(g_recent_evt);
}

void WINAPI MySHAddToRecentDocs(UINT uFlags, LPCVOID pv) {
  // Forward to the real implementation first (don't break Chrome's own
  // recent-docs tracking).
  if (RealSHAddToRecentDocs) RealSHAddToRecentDocs(uFlags, pv);
  CaptureRecentFromFlags(uFlags, pv);
}

// Worker thread: periodically re-registers the jump list so the "Recent"
// category reflects Chrome's latest History (Chrome does NOT surface its
// recently-closed tabs through SHAddToRecentDocs, so we read History directly
// on a timer). RegisterTasks is self-contained (CoInit/CoUninit +
// BeginList/CommitList under g_reg_cs).
static DWORD WINAPI RecentFlushThread(LPVOID) {
  for (;;) {
    Sleep(45000);  // refresh every 45s
    AumidCsInit();
    EnterCriticalSection(&g_aumid_cs);
    std::wstring aid = g_resolved_aumid;
    LeaveCriticalSection(&g_aumid_cs);
    if (!aid.empty()) {
      if (config.IsFixTaskbarMenu()) {
        RegisterTasks(aid);
      } else {
        // If the user toggled the feature off without restarting, the initial
        // startup already skipped registration; periodically clear any list
        // that may have re-appeared so the menu stays as "Unpin" only.
        ClearJumpList(aid);
      }
    }
  }
  return 0;
}

static void InstallRecentDocsHook() {
  if (RealSHAddToRecentDocs) return;  // idempotent
  RecentCsInit();
  if (!g_recent_evt)
    g_recent_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (g_recent_evt) {
    HANDLE h = CreateThread(nullptr, 0, RecentFlushThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
  }
  RealSHAddToRecentDocs = &SHAddToRecentDocs;
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<PVOID*>(&RealSHAddToRecentDocs),
               reinterpret_cast<PVOID>(MySHAddToRecentDocs));
  DetourTransactionCommit();
}

// ---------------------------------------------------------------------------
// Config port (unrelated to AUMID, kept here for single-file convenience).
// ---------------------------------------------------------------------------

namespace {

// FNV-1a 32-bit over the install path (canonical algorithm; case-sensitive).
uint32_t Fnv1a32(const std::wstring& s) {
  uint32_t h = 2166136261u;
  for (wchar_t c : s) {
    h ^= static_cast<uint32_t>(c);
    h *= 16777619u;
  }
  return h;
}

}  // namespace

int GetConfigPort() {
  // Derive a stable per-install port for the local config HTTP server.
  //
  // Without this, every portable Chrome would bind (and point its UI links
  // at) the same hardcoded port 8090. Two installs then collide: the first to
  // start owns the server, and the second's config page — whose URL is baked
  // into the browser UI by pakpatch — actually talks to the FIRST install's
  // server. That routes one instance's update/apply actions to another
  // instance, exactly the "I updated chrome #2 but chrome #1's window popped
  // up and #2 never finished" bug. Deriving the port from the install path
  // keeps each installation isolated to its own port.
  //
  // The same value must be produced by pakpatch (at DLL load, to bake the
  // correct link) and by the HTTP server (when it binds), which is guaranteed
  // because both run in the same process and use the same install directory.
  std::wstring dir = GetSelfDllDir();
  uint32_t h = Fnv1a32(dir);
  int port = 8090 + (int)(h % 1900u);  // spread across 8090..9989
  if (port < 1024) port = 8090;
  return port;
}

// Index of the incognito (stealth) icon within chrome.exe's RT_GROUP_ICON
// table. Confirmed visually (hat-and-glasses on dark background) at index 7 for
// current Chrome builds.
int GetIncognitoIconIndex(const std::wstring& exe) {
  HICON probe = nullptr;
  if (ExtractIconExW(exe.c_str(), 7, &probe, nullptr, 1) == 1 && probe) {
    DestroyIcon(probe);
    return 7;
  }
  return 1;  // fallback if index 7 is missing
}

// Register jump-list Tasks (New Window / Incognito) that point at THIS
// install's chrome.exe, under the given AUMID (should be Chrome's own, so
// the taskbar groups us correctly with pinned shortcuts).
static void RegisterTasks(const std::wstring& aumid) {
  if (aumid.empty()) return;

  RegCsInit();
  EnterCriticalSection(&g_reg_cs);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  ICustomDestinationList* pdl = nullptr;
  UINT min_slots = 0;
  IObjectArray* removed = nullptr;

  HRESULT hr =
      CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_ALL,
                       IID_ICustomDestinationList, (void**)&pdl);
  if (FAILED(hr) || !pdl) { LeaveCriticalSection(&g_reg_cs); CoUninitialize(); return; }

  pdl->SetAppID(aumid.c_str());

  hr = pdl->BeginList(&min_slots, IID_IObjectArray, (void**)&removed);
  if (FAILED(hr)) { LeaveCriticalSection(&g_reg_cs); pdl->Release(); CoUninitialize(); return; }
  if (removed) removed->Release();

  IObjectCollection* tasks = nullptr;
  hr = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_ALL,
                        IID_IObjectCollection, (void**)&tasks);
  if (FAILED(hr) || !tasks) { LeaveCriticalSection(&g_reg_cs); pdl->CommitList(); pdl->Release(); CoUninitialize(); return; }

  const std::wstring exe_path = GetSelfDllDir() + L"\\chrome.exe";

  auto add_task = [&](const wchar_t* title, const wchar_t* args,
                       int icon_index) -> void {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_ALL,
                                IID_IShellLinkW, (void**)&sl)))
      return;
    sl->SetPath(exe_path.c_str());
    sl->SetArguments(args);
    sl->SetIconLocation(exe_path.c_str(), icon_index);
    sl->SetWorkingDirectory(GetSelfDllDir().c_str());

    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPropertyStore, (void**)&ps)) && ps) {
      const size_t len = wcslen(title) + 1;
      wchar_t* buf =
          static_cast<wchar_t*>(CoTaskMemAlloc(len * sizeof(wchar_t)));
      if (buf) {
        memcpy(buf, title, len * sizeof(wchar_t));
        PROPVARIANT pv;
        pv.vt = VT_LPWSTR;
        pv.pwszVal = buf;
        ps->SetValue(PKEY_Title, pv);
        PropVariantClear(&pv);
      }
      ps->Commit();
      ps->Release();
    }
    tasks->AddObject(sl);
    sl->Release();
  };

  const std::wstring locale = GetChromeLocale();
  const wchar_t* lbl_nw = L"New window";
  const wchar_t* lbl_ni = L"New Incognito window";
  const wchar_t* lbl_recent = L"Recent";
  PickLabels(locale, lbl_nw, lbl_ni, lbl_recent);

  add_task(lbl_nw, L"", 0);
  add_task(lbl_ni, L"--incognito", GetIncognitoIconIndex(exe_path));

  IObjectArray* oa = nullptr;
  if (SUCCEEDED(tasks->QueryInterface(IID_IObjectArray, (void**)&oa))) {
    pdl->AddUserTasks(oa);
    oa->Release();
  }
  tasks->Release();

  // --- Append "Recent" category from Chrome's History (recently opened tabs) ---
  {
    std::vector<sqlite_hist::Entry> entries;
    ReadRecentEntries(entries);
    int rcnt = (int)entries.size();

    if (rcnt > 0) {
      IObjectCollection* recent_objs = nullptr;
      HRESULT rhr = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                     CLSCTX_ALL, IID_IObjectCollection,
                                     (void**)&recent_objs);
      if (SUCCEEDED(rhr) && recent_objs) {
        int faviconsUsed = 0;
        for (int i = 0; i < rcnt; ++i) {
          IShellLinkW* rsl = nullptr;
          if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_ALL,
                                      IID_IShellLinkW, (void**)&rsl)))
            continue;
          rsl->SetPath(exe_path.c_str());
          rsl->SetArguments(entries[i].url.c_str());
          rsl->SetWorkingDirectory(GetSelfDllDir().c_str());

          // Use the site's favicon as the icon when available (like Chrome's
          // native Recent list).  When no favicon is found we intentionally do
          // NOT call SetIconLocation — Windows always shows *some* icon (it
          // defaults to the target exe, i.e. chrome.exe), so the item simply
          // shows the site title with the default chrome icon instead of a
          // misleading forced icon.  There is no way to have a truly blank icon.
          if (!g_favicons_path.empty()) {
            std::vector<uint8_t> favicon_png;
            if (sqlite_hist::ReadFaviconPng(g_favicons_path,
                                             entries[i].url,
                                             favicon_png)) {
              std::wstring icoPath = WriteFaviconIco(favicon_png, i);
              if (!icoPath.empty()) {
                rsl->SetIconLocation(icoPath.c_str(), 0);
                faviconsUsed++;
              }
            }
          }

          IPropertyStore* rps = nullptr;
          if (SUCCEEDED(rsl->QueryInterface(IID_IPropertyStore,
                                            (void**)&rps)) &&
              rps) {
            const std::wstring& rtitle = entries[i].title;
            const size_t rlen = rtitle.length() + 1;
            wchar_t* rbuf = static_cast<wchar_t*>(
                CoTaskMemAlloc(rlen * sizeof(wchar_t)));
            if (rbuf) {
              memcpy(rbuf, rtitle.c_str(), rlen * sizeof(wchar_t));
              PROPVARIANT rpv;
              rpv.vt = VT_LPWSTR;
              rpv.pwszVal = rbuf;
              rps->SetValue(PKEY_Title, rpv);
              PropVariantClear(&rpv);
            }
            rps->Commit();
            rps->Release();
          }
          recent_objs->AddObject(rsl);
          rsl->Release();
        }

        IObjectArray* roa = nullptr;
        if (SUCCEEDED(recent_objs->QueryInterface(IID_IObjectArray,
                                                  (void**)&roa))) {
          HRESULT ahr = pdl->AppendCategory(lbl_recent, roa);
          AddDebugLog("appid: Recent category items=" + std::to_string(rcnt) +
                      " favicons=" + std::to_string(faviconsUsed) +
                      " name=" + WstrToUtf8(lbl_recent) +
                      " append_hr=" + std::to_string(static_cast<long>(ahr)));
          roa->Release();
        }
        recent_objs->Release();
      } else {
        AddDebugLog("appid: Recent category: failed to create collection");
      }
    } else {
      AddDebugLog(std::string("appid: Recent category: no items (History "
                              "empty/unreadable) path=") +
                  WstrToUtf8(g_history_path) +
                  (FileExists(g_history_path.c_str()) ? " (exists)"
                                                      : " (MISSING)"));
    }
  }
  // --- End Recent category ---

  // Only log failures: the Recent refresh thread calls this every 45s.
  HRESULT hr_commit = pdl->CommitList();
  if (FAILED(hr_commit))
    AddDebugLog("appid: RegisterTasks failed commit_hr=" +
                std::to_string(static_cast<long>(hr_commit)));
  pdl->Release();
  CoUninitialize();
  LeaveCriticalSection(&g_reg_cs);
}

// Commit an empty custom destination list under the given AUMID. This removes
// any previously-registered Tasks or "Recent" categories so the right-click
// menu for a pinned shortcut only shows the system "Unpin" item.
static void ClearJumpList(const std::wstring& aumid) {
  if (aumid.empty()) return;

  RegCsInit();
  EnterCriticalSection(&g_reg_cs);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  ICustomDestinationList* pdl = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_ALL,
                                IID_ICustomDestinationList, (void**)&pdl);
    if (SUCCEEDED(hr) && pdl) {
    pdl->SetAppID(aumid.c_str());
    UINT min_slots = 0;
    IObjectArray* removed = nullptr;
    hr = pdl->BeginList(&min_slots, IID_IObjectArray, (void**)&removed);
    if (SUCCEEDED(hr)) {
      if (removed) removed->Release();
      hr = pdl->CommitList();
      AddDebugLog("appid: cleared jump list (commit_hr=" +
                  std::to_string(static_cast<long>(hr)) + ")");
    } else {
      AddDebugLog("appid: ClearJumpList BeginList failed hr=" +
                  std::to_string(static_cast<long>(hr)));
    }
    pdl->Release();
  }

  // Also drop any automatic/Recent destinations so the menu truly has
  // nothing but the system "Unpin" entry.
  IApplicationDestinations* ad = nullptr;
  hr = CoCreateInstance(CLSID_ApplicationDestinations, nullptr, CLSCTX_ALL,
                        IID_IApplicationDestinations, (void**)&ad);
  if (SUCCEEDED(hr) && ad) {
    ad->SetAppID(aumid.c_str());
    ad->RemoveAllDestinations();
    ad->Release();
  }

  CoUninitialize();
  LeaveCriticalSection(&g_reg_cs);
}

// Write our AUMID onto the chrome.exe FILE so a shortcut pinned from Explorer
// inherits it (a pin takes its AppUserModelID from the exe's property, else
// falls back to an implicit path-based id). Persists on the file, so it only
// has to succeed once — the updater does it while the exe is unlocked; here it
// usually fails because the running exe is image-locked, which is harmless.
static void StampExeAumid(const std::wstring& exe, const std::wstring& aumid) {
  IPropertyStore* ps = nullptr;
  HRESULT hr = SHGetPropertyStoreFromParsingName(
      exe.c_str(), nullptr, GPS_READWRITE, IID_IPropertyStore, (void**)&ps);
  if (FAILED(hr) || !ps) return;
  PROPVARIANT pv;
  ZeroMemory(&pv, sizeof(pv));
  pv.vt = VT_LPWSTR;
  pv.pwszVal = const_cast<wchar_t*>(aumid.c_str());
  // Do NOT PropVariantClear — pwszVal points at aumid's buffer, not CoTaskMem.
  if (SUCCEEDED(ps->SetValue(PKEY_AppUserModel_ID, pv))) ps->Commit();
  ps->Release();
}

// Read the AUMID the updater stamped onto chrome.exe. Reusing that exact value
// (instead of recomputing FNV) keeps window, process and .lnk on one identity.
// GPS_DEFAULT (read-only) so it works while the exe is image-locked. Empty if
// the exe has no stamp — caller falls back to FNV of the install dir.
static std::wstring ReadExeAumid(const std::wstring& exe) {
  IPropertyStore* ps = nullptr;
  HRESULT hr = SHGetPropertyStoreFromParsingName(
      exe.c_str(), nullptr, GPS_DEFAULT, IID_IPropertyStore, (void**)&ps);
  if (FAILED(hr) || !ps) return std::wstring();
  PROPVARIANT pv = {};
  std::wstring out;
  if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR &&
      pv.pwszVal && pv.pwszVal[0]) {
    out = pv.pwszVal;
  }
  PropVariantClear(&pv);
  ps->Release();
  return out;
}

// ---------------------------------------------------------------------------
// Stale ENGLISH jump-list cleanup.
//
// Old Chrome builds registered their English Tasks under the exe-path-derived
// implicit identity, and Windows keeps that list forever under
// %APPDATA%\Microsoft\Windows\Recent\{Custom,Automatic}Destinations. A pin
// made from an unstamped chrome.exe has no AUMID, so it resolves to that
// identity and shows the stale English menu even with Chrome closed. Chrome
// now writes to ChromeGreen.<hash> instead, so deleting it once is permanent.
//
// Match on BOTH our exe path and an English label (our own lists are localized
// and never contain them). Strings in these OLE files may sit at ODD byte
// offsets, so match raw bytes rather than decoding as UTF-16.
static bool BlobContainsUtf16(const std::string& blob, const wchar_t* ws) {
  const char* p = reinterpret_cast<const char*>(ws);
  size_t n = wcslen(ws) * sizeof(wchar_t);
  return blob.find(std::string(p, n)) != std::string::npos;
}

static void CleanStaleChromeJumpLists(const std::wstring& exe) {
  wchar_t appdata[MAX_PATH] = {0};
  DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;

  const std::wstring dirs[] = {
      std::wstring(appdata) +
          L"\\Microsoft\\Windows\\Recent\\CustomDestinations",
      std::wstring(appdata) +
          L"\\Microsoft\\Windows\\Recent\\AutomaticDestinations",
  };

  int removed = 0;
  for (const auto& dir : dirs) {
    WIN32_FIND_DATAW fd = {0};
    HANDLE h = FindFirstFileW((dir + L"\\*Destinations-ms").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow > 4u * 1024 * 1024) continue;

      std::wstring path = dir + L"\\" + fd.cFileName;
      HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
      if (f == INVALID_HANDLE_VALUE) continue;
      std::string blob;
      char buf[8192];
      DWORD rd = 0;
      while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd)
        blob.append(buf, rd);
      CloseHandle(f);

      // English labels => Chrome-authored list; our exe path => it belongs to
      // THIS install's implicit identity. Both must match.
      if (!BlobContainsUtf16(blob, L"New Window") &&
          !BlobContainsUtf16(blob, L"Incognito Window"))
        continue;
      if (!BlobContainsUtf16(blob, exe.c_str())) continue;

      if (DeleteFileW(path.c_str())) removed++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  if (removed)
    AddDebugLog("appid: removed stale EN jump lists=" +
                std::to_string(removed));
}

// Scan one taskbar-pin folder for .lnk files whose target is `exe` and stamp
// them with `aumid`; returns the number patched.
static int PatchLnkDir(const std::wstring& dir, const std::wstring& exe,
                       const std::wstring& aumid) {
  std::wstring pattern = dir + L"\\*.lnk";
  WIN32_FIND_DATAW fd = {0};
  HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) return 0;

  int fixed = 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

    std::wstring lnk_path = dir + L"\\" + fd.cFileName;

    IShellLinkW* sl = nullptr;
    IPersistFile* pf = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_ALL,
                                  IID_IPersistFile, (void**)&pf);
    if (FAILED(hr) || !pf) continue;
    hr = pf->Load(lnk_path.c_str(), STGM_READWRITE);
    if (SUCCEEDED(hr))
      hr = pf->QueryInterface(IID_IShellLinkW, (void**)&sl);
    if (FAILED(hr) || !sl) { pf->Release(); continue; }

    wchar_t target[MAX_PATH] = {0};
    sl->GetPath(target, MAX_PATH, nullptr, 0);

    // Case-insensitive compare (Windows paths are case-insensitive).
    if (_wcsicmp(target, exe.c_str()) == 0) {
      IPropertyStore* ps = nullptr;
      hr = sl->QueryInterface(IID_IPropertyStore, (void**)&ps);
      if (SUCCEEDED(hr) && ps) {
        PROPVARIANT pv;
        ZeroMemory(&pv, sizeof(pv));
        pv.vt = VT_LPWSTR;
        pv.pwszVal = const_cast<wchar_t*>(aumid.c_str());
        hr = ps->SetValue(PKEY_AppUserModel_ID, pv);
        if (SUCCEEDED(hr)) hr = ps->Commit();
        ps->Release();
        // Commit() only mutates the in-memory object; Save() flushes to disk.
        if (SUCCEEDED(hr)) {
          hr = pf->Save(nullptr, TRUE);
          AddDebugLog("appid: FixPinnedShortcut save_hr=" +
                      std::to_string(static_cast<long>(hr)));
        }
        // Let Explorer re-read this .lnk so it picks up the new AUMID.
        // Synchronous so the refresh is not lost.
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATH, lnk_path.c_str(), nullptr);
        if (SUCCEEDED(hr)) fixed++;
      }
    }
    sl->Release();
    pf->Release();
  } while (FindNextFileW(hFind, &fd));
  FindClose(hFind);
  return fixed;
}

// Patch the taskbar-pinned .lnk pointing at `exe` so it carries our AUMID and
// groups with the running window. Needed because StampExeAumid cannot write the
// exe while it is locked, so it cannot fix an already-pinned shortcut.
static void FixPinnedShortcutAumid(const std::wstring& exe,
                                   const std::wstring& aumid) {
  // Resolve %APPDATA% directly from the environment — far more reliable than
  // SHGetFolderPathW here (observed to fail in this process).
  wchar_t appdata[MAX_PATH] = {0};
  DWORD ad_len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  if (ad_len == 0 || ad_len >= MAX_PATH) {
    AddDebugLog("appid: FixPinnedShortcut no APPDATA env");
    return;
  }
  std::wstring appdata_dir(appdata);

  wchar_t localappdata[MAX_PATH] = {0};
  DWORD la_len = GetEnvironmentVariableW(L"LOCALAPPDATA", localappdata, MAX_PATH);
  std::wstring localappdata_dir =
      (la_len && la_len < MAX_PATH) ? std::wstring(localappdata) : appdata_dir;

  // Candidate folders where taskbar-pinned shortcuts may live.
  std::wstring candidates[] = {
      appdata_dir +
          L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar",
      localappdata_dir + L"\\Microsoft\\Windows\\Application Shortcuts",
  };

  int total = 0;
  for (const auto& dir : candidates) {
    total += PatchLnkDir(dir, exe, aumid);
  }

  AddDebugLog("appid: FixPinnedShortcut patched=" + std::to_string(total));
}

// Last-resort profile lookup: pick the <user-data-dir>\<profile>\History file
// with the newest write time. That is the profile Chrome is actually using even
// when it is not "Default" and no --profile-directory switch was passed.
static std::wstring FindNewestHistory(const std::wstring& user_data_dir) {
  if (user_data_dir.empty()) return L"";
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((user_data_dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return L"";
  std::wstring best;
  FILETIME best_time{};
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (fd.cFileName[0] == L'.') continue;
    std::wstring hp =
        user_data_dir + L"\\" + fd.cFileName + L"\\History";
    WIN32_FIND_DATAW hfd;
    HANDLE hh = FindFirstFileW(hp.c_str(), &hfd);
    if (hh == INVALID_HANDLE_VALUE) continue;
    FindClose(hh);
    if (best.empty() || CompareFileTime(&hfd.ftLastWriteTime, &best_time) > 0) {
      best = hp;
      best_time = hfd.ftLastWriteTime;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return best;
}

// Resolve the path to Chrome's History SQLite DB. We are loaded into the
// chrome.exe process, so GetCommandLineW gives the real launch args (with the
// portable --user-data-dir injection applied). Switches are parsed with
// GetCmdArgValue so values containing spaces survive.
static std::wstring GetHistoryPath() {
  std::wstring ud = GetCmdArgValue(L"--user-data-dir=");
  std::wstring base;
  if (!ud.empty()) {
    base = ud;
  } else {
    wchar_t la[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n && n < MAX_PATH)
      base = std::wstring(la) + L"\\Google\\Chrome\\User Data";
    else
      return L"";  // unknown location
  }
  if (base.empty()) return L"";
  std::wstring prof = GetCmdArgValue(L"--profile-directory=");
  std::wstring hp =
      base + L"\\" + (prof.empty() ? L"Default" : prof) + L"\\History";
  if (FileExists(hp.c_str())) return hp;
  // Wrong profile name (or a renamed/moved profile): fall back to whichever
  // profile was used last instead of giving up on the Recent category.
  std::wstring found = FindNewestHistory(base);
  return found.empty() ? hp : found;
}

// Favicons DB lives in the same profile directory as History.
static std::wstring GetFaviconsPath() {
  std::wstring hp = GetHistoryPath();
  if (hp.empty()) return L"";
  // Replace trailing "History" with "Favicons"
  if (hp.size() > 7 && hp.compare(hp.size() - 7, 7, L"History") == 0)
    return hp.substr(0, hp.size() - 7) + L"Favicons";
  return L"";
}

// --- Favicon .ico file writer -----------------------------------------------
// Native Chrome stores favicons as PNG and uses PNG-embedded ICO for its
// jump-list icons (supported since Windows Vista).  Windows' jump-list icon
// loader (IShellLink / CreateIconFromResource) handles PNG-in-ICO fine — the
// earlier "no icon" symptom was actually a favicon-LOOKUP miss, not a format
// problem.  So we emit a single-image PNG-in-ICO with NO re-encoding: zero
// quality loss, no GDI+ dependency, and the original dimensions are preserved
// (Windows scales to the slot size as needed).  Returns the .ico path, or
// empty on failure.
static std::wstring WriteFaviconIco(const std::vector<uint8_t>& png_data,
                                    int index) {
  // Need a valid PNG: 8-byte signature + IHDR (width/height at bytes 16..23).
  if (png_data.size() < 24 ||
      png_data[0] != 0x89 || png_data[1] != 0x50 ||
      png_data[2] != 0x4E || png_data[3] != 0x47)
    return L"";
  // PNG IHDR: width/height are 4-byte big-endian at offset 16 / 20.
  uint32_t w = ((uint32_t)png_data[16] << 24) | ((uint32_t)png_data[17] << 16) |
               ((uint32_t)png_data[18] << 8)  |  (uint32_t)png_data[19];
  uint32_t h = ((uint32_t)png_data[20] << 24) | ((uint32_t)png_data[21] << 16) |
               ((uint32_t)png_data[22] << 8)  |  (uint32_t)png_data[23];
  if (w == 0 || h == 0 || w > 256 || h > 256) return L"";

  // ChromeGreen data directory (config cg_data_dir, defaults to cache dir);
  // its fixed ChromeGreenData\favicons subfolder holds the icons. Falls back
  // to <DLL>\..\Cache\ChromeGreenData if the config key is unset.
  std::wstring base = Config::Instance().GetCgDataRoot().value_or(
      GetSelfDllDir() + L"\\..\\Cache\\ChromeGreenData");
  CreateDirectoryW(base.c_str(), nullptr);
  std::wstring dir = base + L"\\favicons";
  CreateDirectoryW(dir.c_str(), nullptr);  // ignore err (may exist)
  wchar_t name[64];
  // Use fixed names so we reuse files across refresh cycles (no accumulation).
  // NOTE: swprintf (not wsprintfW) — wsprintfW lives in user32.lib which we
  // don't link; swprintf is already used elsewhere (see SetAppId).
  swprintf(name, 64, L"favicon_%d.ico", index);
  std::wstring path = dir + L"\\" + name;

  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return L"";

  uint32_t pngSize = static_cast<uint32_t>(png_data.size());

  // ICONDIR: reserved(0,2) + type=1 icon(2) + count=1(2)
  uint8_t icoHdr[6] = {0, 0, 1, 0, 1, 0};

  // ICONDIRENTRY (16 bytes)
  uint8_t entry[16] = {0};
  entry[0] = (w >= 256) ? 0 : (uint8_t)w;   // bWidth  (0 == 256)
  entry[1] = (h >= 256) ? 0 : (uint8_t)h;   // bHeight (0 == 256)
  entry[2] = 0;   // bColorCount (0 for >8bpp / PNG)
  entry[3] = 0;   // bReserved
  entry[4] = 1; entry[5] = 0;   // wPlanes = 1
  entry[6] = 32; entry[7] = 0;  // wBitCount = 32 (PNG is effectively 32bpp)
  entry[8]  = (uint8_t)(pngSize & 0xFF);
  entry[9]  = (uint8_t)((pngSize >> 8) & 0xFF);
  entry[10] = (uint8_t)((pngSize >> 16) & 0xFF);
  entry[11] = (uint8_t)((pngSize >> 24) & 0xFF);
  entry[12] = 22; entry[13] = 0; entry[14] = 0; entry[15] = 0;  // dwImageOffset = 22

  DWORD written;
  BOOL ok = TRUE;
  ok &= WriteFile(hFile, icoHdr, 6, &written, nullptr);
  ok &= WriteFile(hFile, entry, 16, &written, nullptr);
  ok &= WriteFile(hFile, png_data.data(), pngSize, &written, nullptr);
  CloseHandle(hFile);
  if (!ok) { DeleteFileW(path.c_str()); return L""; }
  return path;
}

// Build the Recent category entries: primarily from Chrome's History DB
// (recently opened tabs); fall back to anything captured via the (rarely used)
// SHAddToRecentDocs hook. Capped at kMaxRecentItems.
static void ReadRecentEntries(std::vector<sqlite_hist::Entry>& out) {
  out.clear();
  if (!g_history_path.empty()) {
    std::vector<sqlite_hist::Entry> hist;
    if (sqlite_hist::ReadRecentUrls(g_history_path, kMaxRecentItems, hist)) {
      for (auto& e : hist) out.push_back(std::move(e));
    }
  }
  // Fallback: items captured from SHAddToRecentDocs (Chrome usually doesn't
  // call this for tabs, but keep it as a secondary source).
  RecentCsInit();
  EnterCriticalSection(&g_recent_cs);
  for (int i = 0; i < g_recent_count; ++i)
    out.push_back({g_recent_items[i].url, g_recent_items[i].title});
  LeaveCriticalSection(&g_recent_cs);
  if ((int)out.size() > kMaxRecentItems) out.resize(kMaxRecentItems);
}

// Force ONE stable per-install AUMID (ChromeGreen.<hash>) onto the process,
// every Chrome window, and the pinned .lnk, then register our localized Tasks
// under it. Before launch the pin points at an id with no jump list registered,
// so right-click shows only "Unpin".
void SetAppId() {
  // Resolve before installing the hook, which rewrites every window AUMID to
  // this value. Prefer the updater-stamped id so we match the pinned .lnk
  // exactly; otherwise fall back to FNV-1a of the install dir.
  {
    std::wstring stamped = ReadExeAumid(GetSelfDllDir() + L"\\chrome.exe");
    if (!stamped.empty()) {
      g_our_aumid = stamped;
    } else {
      std::wstring dir = GetSelfDllDir();
      if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
      wchar_t buf[40];
      swprintf(buf, 40, L"ChromeGreen.%08X", Fnv1a32(dir));
      g_our_aumid = buf;
    }
  }

  InstallAumidHooks();
  InstallRecentDocsHook();  // capture Chrome's SHAddToRecentDocs for "Recent" category

  // Run off the main thread: wait for Chrome to finish its own jump-list write,
  // then register our Tasks under OUR AUMID.
  HANDLE hThread = CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        // Must init COM here: StampExeAumid / FixPinnedShortcutAumid run before
        // RegisterTasks (which has its own) and need CoCreateInstance.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::wstring aid = g_our_aumid;
        if (aid.empty()) {
          AddDebugLog("appid: no AUMID resolved; skipping task registration");
          return 0;
        }
        g_resolved_aumid = aid;
        g_history_path = GetHistoryPath();
        g_favicons_path = GetFaviconsPath();
        AddDebugLog(std::string("appid: history path=") +
                    WstrToUtf8(g_history_path) +
                    (FileExists(g_history_path.c_str()) ? " (exists)"
                                                        : " (MISSING)"));
        AddDebugLog("appid: favicons path=" + WstrToUtf8(g_favicons_path));
        AddDebugLog("appid: AUMID=" + WstrToUtf8(aid));

        const std::wstring exe = GetSelfDllDir() + L"\\chrome.exe";
        SetCurrentProcessExplicitAppUserModelID(aid.c_str());
        StampExeAumid(exe, aid);           // future pins (idempotent)
        FixPinnedShortcutAumid(exe, aid);  // already-pinned shortcuts

        if (config.IsFixTaskbarMenu()) {
          // Drop the stale English list Chrome left under the implicit identity;
          // it is what a freshly-pinned (unstamped-exe) shortcut would show.
          CleanStaleChromeJumpLists(exe);
          // Let Chrome finish its own jump-list write so ours wins.
          Sleep(2000);
          RegisterTasks(aid);              // localized Tasks + Recent
        } else {
          // Feature off: remove any Tasks/Recent categories that may have been
          // registered for this AUMID in a previous run (or by Chrome before we
          // took over the AUMID). Stale English lists under the implicit
          // exe-path identity are also dropped so a freshly-pinned shortcut
          // cannot inherit them.
          AddDebugLog("appid: fix_taskbar_menu off; clearing jump lists "
                      "for AUMID so only Unpin shows");
          CleanStaleChromeJumpLists(exe);
          ClearJumpList(aid);
        }
        CoUninitialize();
        return 0;
      },
      nullptr, 0, nullptr);
  if (hThread) CloseHandle(hThread);
}
