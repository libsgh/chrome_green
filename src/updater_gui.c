// Chrome Green — standalone update window (no-CRT pure Win32 C).
//
// This is a tiny GUI process (target ~20-40 KB) that performs the
// update/restart sequence. It is embedded as an RCDATA resource inside
// version.dll and extracted to <install>\updates\chrome_green_updater.exe
// at update time.
//
// It performs the update/restart sequence:
//   1. wait for the parent Chrome PID to exit
//   2. wait for any Chrome process under our install dir to exit
//   3. force-kill leftover processes (path-based, never by image name)
//   4. delete the persisted update-state file FIRST (so a manual Chrome
//      launch afterwards is a clean kIdle start)
//   5. swap chrome.exe.new -> chrome.exe
//   6. sweep .cg_orphan_* directories
//   7. copy Chrome-bin -> AppDir (+ old-version handling)
//   8. self-update version.dll (if pending)
//   9. clean update_temp
//  10. start Chrome (up to 3 attempts)
//
// The window shows a single progress bar + the current step text and a
// close [x] button that force-aborts the sequence and launches Chrome.
//
// No C runtime is used: entry is wWinMain, only kernel32/user32/gdi32 are
// linked, and memcpy/memset are provided as tiny stubs.

#include <windows.h>
#include <tlhelp32.h>

// No debug logging in release builds.

#define WIN_W 440
#define WIN_H 108
#define IDR_UPDATER_EXE 1001

#define WM_APP_STEP (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

// ---- globals ----
static volatile LONG g_abort = 0;
static int g_chrome_launched = 0;
static HWND g_hwnd = NULL;
static int g_percent = 0;
static int g_last_post = -1;
static WCHAR g_status[128];
static RECT g_closeRect = { WIN_W - 48, 12, WIN_W - 20, 40 };

static WCHAR g_app_dir[520];
static WCHAR g_self_dll_dir[520];
static WCHAR g_module[520];
static WCHAR g_manifest[520];
// 0 = real update flow (a manifest was extracted alongside us); 1 = preview
// (--test/--demo, or launched with no manifest). Set in wWinMain.
static int g_test_mode = 0;
static WCHAR g_temp[520];
static DWORD g_pid = 0;
static WCHAR g_chrome_exe[520];
static int g_keep_old = 0;
static int g_keep_installer = 0;
static WCHAR g_keep_version[64];
static int g_self_update_ready = 0;
static WCHAR g_self_dll_new[520];
static WCHAR g_state_file[520];

// ---- theme (kept in sync with the config page) ----
// g_theme holds the raw value from the manifest: "auto" / "light" / "dark".
// The window resolves it (auto -> OS preference) into a concrete light/dark
// palette that mirrors web/src/style.css :root / .dark variables.
// Forward declaration: wcmpI is defined further below in the wide-string helpers.
static int wcmpI(const WCHAR* a, const WCHAR* b);
static WCHAR g_theme[16];

typedef struct {
  COLORREF bg;        // window background (--background)
  COLORREF border;    // window border (--border)
  COLORREF title;     // title text (--foreground)
  COLORREF muted;     // percentage / close glyph (--muted-foreground)
  COLORREF status;    // current-step text (--foreground, slightly softened)
  COLORREF track;     // progress track
  COLORREF fill;      // progress fill (neutral --primary, matches config page)
} Palette;
static Palette g_pal;
static int g_dark = 0; // 0 = light, 1 = dark

static void set_palette(int dark) {
  g_dark = dark;
  if (dark) {
    g_pal.bg      = RGB(10, 10, 10);     // --background  0 0% 3.9%  (neutral near-black)
    g_pal.border  = RGB(38, 38, 38);     // --border      0 0% 14.9%
    g_pal.title   = RGB(250, 250, 250);  // --foreground  0 0% 98%
    g_pal.muted   = RGB(163, 163, 163);  // --muted-fg    0 0% 63.9%
    g_pal.status  = RGB(228, 228, 228);  // --foreground, softened (~0 0% 89%)
    g_pal.track   = RGB(38, 38, 38);     // --secondary   0 0% 14.9%
    g_pal.fill    = RGB(250, 250, 250);  // --primary     0 0% 98% (neutral, matches config page)
  } else {
    g_pal.bg      = RGB(255, 255, 255);  // --background  0 0% 100%
    g_pal.border  = RGB(229, 229, 229);  // --border      0 0% 89.8%
    g_pal.title   = RGB(10, 10, 10);     // --foreground  0 0% 3.9%
    g_pal.muted   = RGB(115, 115, 115);  // --muted-fg    0 0% 45.1%
    g_pal.status  = RGB(64, 64, 64);     // --foreground, softened (~0 0% 25%)
    g_pal.track   = RGB(245, 245, 245);  // --secondary   0 0% 96.1%
    g_pal.fill    = RGB(23, 23, 23);     // --primary     0 0% 9% (neutral, matches config page)
  }
}

// Mirror the config page's "auto" logic: prefers-color-scheme: dark, which on
// Windows follows HKCU\...\Personalize\AppsUseLightTheme (0 = dark, 1 = light).
static int system_is_dark(void) {
  HKEY hk;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hk) != ERROR_SUCCESS) {
    return 0; // key missing -> assume light
  }
  DWORD v = 1, sz = sizeof(v);
  if (RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&v, &sz) != ERROR_SUCCESS) {
    v = 1; // missing value -> assume light
  }
  RegCloseKey(hk);
  return (v == 0) ? 1 : 0;
}

static void resolve_theme(void) {
  int dark;
  if (wcmpI(g_theme, L"dark") == 0) { dark = 1; set_palette(1); }
  else if (wcmpI(g_theme, L"light") == 0) { dark = 0; set_palette(0); }
  else { dark = system_is_dark(); set_palette(dark); }
}

static long long g_total = 1;
static long long g_copied_base = 0;

// Forward declaration (used by CopyCb and Worker below).
static void set_status(int pct, const WCHAR* txt);

// ---- CRT stubs ----
void* memcpy(void* d, const void* s, size_t n) {
  unsigned char* dd = (unsigned char*)d;
  const unsigned char* ss = (const unsigned char*)s;
  for (size_t i = 0; i < n; i++) dd[i] = ss[i];
  return d;
}
void* memset(void* d, int c, size_t n) {
  unsigned char* dd = (unsigned char*)d;
  for (size_t i = 0; i < n; i++) dd[i] = (unsigned char)c;
  return d;
}

// Normally supplied by the CRT; required because this module now uses
// floating-point (REAL / float) for the DPI-scaled, anti-aliased rendering.
// __attribute__((used)) keeps LTO from discarding it (the compiler only
// references it implicitly).
__attribute__((used)) int _fltused = 0;

// ---- wide-string helpers (no CRT) ----
static int wlen(const WCHAR* s) { int n = 0; while (s[n]) n++; return n; }
static void wcpy(WCHAR* d, const WCHAR* s) {
  int i = 0; while (s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}
static void wcat(WCHAR* d, const WCHAR* s) {
  int i = wlen(d), j = 0; while (s[j]) { d[i + j] = s[j]; j++; } d[i + j] = 0;
}
static WCHAR* wchr(const WCHAR* s, WCHAR c) {
  while (*s) { if (*s == c) return (WCHAR*)s; s++; } return NULL;
}
static WCHAR tolow(WCHAR c) { if (c >= L'A' && c <= L'Z') return (WCHAR)(c + 32); return c; }
static int wncmpI(const WCHAR* a, const WCHAR* b, int n) {
  for (int i = 0; i < n; i++) {
    WCHAR ca = tolow(a[i]), cb = tolow(b[i]);
    if (ca < cb) return -1;
    if (ca > cb) return 1;
    if (a[i] == 0) break;
  }
  return 0;
}
static int wcmpI(const WCHAR* a, const WCHAR* b) {
  int i = 0;
  while (a[i] || b[i]) {
    WCHAR ca = tolow(a[i]), cb = tolow(b[i]);
    if (ca < cb) return -1;
    if (ca > cb) return 1;
    i++;
  }
  return 0;
}
static unsigned long wtoul(const WCHAR* s) {
  unsigned long v = 0;
  while (*s >= L'0' && *s <= L'9') { v = v * 10 + (unsigned long)(*s - L'0'); s++; }
  return v;
}
static void ultow(unsigned long v, WCHAR* b) {
  if (v == 0) { b[0] = L'0'; b[1] = 0; return; }
  WCHAR t[24]; int i = 0;
  while (v) { t[i++] = (WCHAR)(L'0' + (v % 10)); v /= 10; }
  int j = 0; while (i > 0) { b[j++] = t[--i]; } b[j] = 0;
}

// ---- path helpers ----
static int pjoin(WCHAR* dst, int cap, const WCHAR* base, const WCHAR* rel) {
  int bl = wlen(base);
  if (bl >= cap - 2) bl = cap - 2;
  wcpy(dst, base);
  if (bl > 0 && dst[bl - 1] != L'\\' && dst[bl - 1] != L'/') { dst[bl] = L'\\'; bl++; }
  const WCHAR* r = rel;
  while (*r == L'\\' || *r == L'/') r++;
  int ri = 0;
  while (r[ri] && bl + ri < cap - 1) { dst[bl + ri] = r[ri]; ri++; }
  dst[bl + ri] = 0;
  return bl + ri;
}

static int file_exists(const WCHAR* p) {
  DWORD a = GetFileAttributesW(p);
  return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}
static int dir_exists(const WCHAR* p) {
  DWORD a = GetFileAttributesW(p);
  return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}
static int is_dot(const WCHAR* n) {
  return (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0)));
}
static int is_version_dir(const WCHAR* s) {
  int dots = 0, digits = 0, i = 0;
  while (s[i]) {
    if (s[i] == L'.') { if (digits == 0) return 0; dots++; digits = 0; }
    else if (s[i] >= L'0' && s[i] <= L'9') digits++;
    else return 0;
    i++;
  }
  return (dots == 3 && digits > 0) ? 1 : 0;
}
static int is_under(const WCHAR* img, const WCHAR* base) {
  int bl = wlen(base);
  if (wncmpI(img, base, bl) != 0) return 0;
  return (img[bl] == L'\\' || img[bl] == L'/') ? 1 : 0;
}

// ---- file operations ----
static void rmtree(const WCHAR* path) {
  SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
  WCHAR search[600];
  pjoin(search, 600, path, L"*");
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (is_dot(fd.cFileName)) continue;
      WCHAR child[600];
      pjoin(child, 600, path, fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(child);
      else { SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL); DeleteFileW(child); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
  RemoveDirectoryW(path);
}

static long long count_bytes(const WCHAR* path) {
  long long total = 0;
  WCHAR search[600];
  pjoin(search, 600, path, L"*");
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (is_dot(fd.cFileName)) continue;
      WCHAR child[600];
      pjoin(child, 600, path, fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += count_bytes(child);
      else total += ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return total;
}

static DWORD CALLBACK CopyCb(LARGE_INTEGER TotalFileSize,
                             LARGE_INTEGER TotalBytesTransferred,
                             LARGE_INTEGER StreamSize, LARGE_INTEGER StreamBytesTransferred,
                             DWORD dwStreamNumber, DWORD dwCallbackReason,
                             HANDLE hSourceFile, HANDLE hDestinationFile, LPVOID lpData) {
  (void)TotalFileSize; (void)StreamSize; (void)StreamBytesTransferred;
  (void)dwStreamNumber; (void)dwCallbackReason; (void)hSourceFile;
  (void)hDestinationFile; (void)lpData;
  if (g_abort) return PROGRESS_CANCEL;
  long long cur = g_copied_base + TotalBytesTransferred.QuadPart;
  // Percentage without a 64-bit division: the no-CRT x86 build has no
  // __alldiv helper. Downshift both operands by 11 bits so they fit in 32
  // bits; the ratio (and thus the percentage) is preserved to ~0.05%.
  int pct = 100;
  if (g_total > 0) {
    unsigned long c = (unsigned long)(cur >> 11);
    unsigned long t = (unsigned long)(g_total >> 11);
    pct = t ? (int)(c * 100UL / t) : 100;
  }
  if (pct > 100) pct = 100;
  int overall = 18 + (pct * 60) / 100; // map copy phase to 18..78 %
  WCHAR buf[64];
  wcpy(buf, L"正在复制文件… ");
  WCHAR num[12]; ultow((unsigned)pct, num); wcat(num, L"%"); wcat(buf, num);
  set_status(overall, buf);
  return PROGRESS_CONTINUE;
}

static int copy_tree(const WCHAR* src, const WCHAR* dst) {
  if (!dir_exists(dst)) CreateDirectoryW(dst, NULL);
  WCHAR search[600];
  pjoin(search, 600, src, L"*");
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  int rc = 0;
  do {
    if (is_dot(fd.cFileName)) continue;
    if (g_abort) { rc = 1; break; }
    WCHAR s[600], d[600];
    pjoin(s, 600, src, fd.cFileName);
    pjoin(d, 600, dst, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      rc = copy_tree(s, d);
      if (rc) break;
    } else {
      SetFileAttributesW(d, FILE_ATTRIBUTE_NORMAL);
      if (!CopyFileExW(s, d, CopyCb, NULL, NULL, 0)) {
        WCHAR dnew[700]; wcpy(dnew, d); wcat(dnew, L".new");
        MoveFileExW(d, dnew, MOVEFILE_REPLACE_EXISTING);
        CopyFileExW(s, d, CopyCb, NULL, NULL, 0);
        DeleteFileW(dnew);
      }
      g_copied_base += ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return rc;
}

// ---- process helpers ----
static int enum_our(DWORD* out, int max) {
  int n = 0;
  DWORD self_pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == self_pid) continue; // never target ourselves
      HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
      if (hp) {
        WCHAR img[600]; DWORD sz = 600;
        if (QueryFullProcessImageNameW(hp, 0, img, &sz)) {
          if (is_under(img, g_app_dir)) {
            if (n < max) out[n++] = pe.th32ProcessID;
          }
        }
        CloseHandle(hp);
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return n;
}
static void kill_pid(DWORD pid) {
  if (!pid) return;
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (h) { TerminateProcess(h, 1); CloseHandle(h); }
}
static void kill_our() {
  DWORD pids[256];
  int n = enum_our(pids, 256);
  for (int i = 0; i < n; i++) kill_pid(pids[i]);
}
static int our_running() { DWORD p[8]; return enum_our(p, 8) > 0; }

static void wait_pid_exit(DWORD pid, int rounds) {
  for (int i = 0; i < rounds; i++) {
    if (g_abort) return;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return; // already gone
    DWORD r = WaitForSingleObject(h, 1000);
    CloseHandle(h);
    if (r == WAIT_OBJECT_0) return;
  }
}
static void wait_our_exit(int rounds) {
  for (int i = 0; i < rounds; i++) {
    if (g_abort) return;
    if (our_running() == 0) return;
    Sleep(1000);
  }
}

// ---- update steps ----
static void swap_chrome_new() {
  WCHAR src[600], dst[600], old[600];
  pjoin(src, 600, g_app_dir, L"chrome.exe.new");
  pjoin(dst, 600, g_app_dir, L"chrome.exe");
  pjoin(old, 600, g_app_dir, L"chrome.exe.old");
  DeleteFileW(old);
  if (file_exists(src)) {
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING)) {
      CopyFileExW(src, dst, NULL, NULL, NULL, 0);
      DeleteFileW(src);
    }
  }
}
static void sweep_orphans() {
  WCHAR search[600];
  pjoin(search, 600, g_app_dir, L".cg_orphan_*");
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (is_dot(fd.cFileName)) continue;
    WCHAR p[600];
    pjoin(p, 600, g_app_dir, fd.cFileName);
    rmtree(p);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}
static void handle_old_versions() {
  WCHAR search[600];
  pjoin(search, 600, g_app_dir, L"*");
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.cFileName[0] == L'.') continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (!is_version_dir(fd.cFileName)) continue;
    if (wcmpI(fd.cFileName, g_keep_version) == 0) continue;
    WCHAR dirp[600];
    pjoin(dirp, 600, g_app_dir, fd.cFileName);
    if (g_keep_old) {
      WCHAR olddir[600];
      pjoin(olddir, 600, g_app_dir, L"old");
      CreateDirectoryW(olddir, NULL);
      WCHAR dst[600];
      pjoin(dst, 600, olddir, fd.cFileName);
      rmtree(dst);
      MoveFileExW(dirp, dst, MOVEFILE_WRITE_THROUGH);
    } else {
      rmtree(dirp);
      if (dir_exists(dirp)) {
        WCHAR orphan[700];
        wcpy(orphan, dirp);
        wcat(orphan, L".cg_orphan_");
        wcat(orphan, fd.cFileName);
        MoveFileExW(dirp, orphan, MOVEFILE_WRITE_THROUGH);
      }
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}
static void ensure_chrome_visible(void);

static void launch_chrome(int verify) {
  if (g_chrome_launched) return;
  WCHAR launch[600];
  pjoin(launch, 600, g_app_dir, L"chrome.exe");
  if (!file_exists(launch)) wcpy(launch, g_chrome_exe);
  for (int attempt = 0; attempt < 3; attempt++) {
    if (our_running()) break;
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    WCHAR cmd[700];
    wcpy(cmd, L"\""); wcat(cmd, launch); wcat(cmd, L"\" --portable");
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, g_app_dir, &si, &pi)) {
      CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    if (!verify) break;
    Sleep(1500);
    if (our_running()) break;
  }
  // Make sure the freshly launched browser window is actually on-screen.
  ensure_chrome_visible();
  g_chrome_launched = 1;
}

// --- Off-screen window recovery ---
// After an update we kill the old Chrome and relaunch it. Chrome restores its
// last saved window position, which can land partially or fully off-screen
// (secondary monitor that's since gone, negative/edge coordinates, etc.). A
// window stuck at the screen edge with its title bar off-screen looks "mostly
// hidden" and can't be dragged. We detect any of THIS install's browser
// windows that are mostly off the visible desktop and move them back on-screen.
static int g_found = 0;
static int g_moved = 0;

static BOOL CALLBACK EnsureVisibleProc(HWND hwnd, LPARAM lp) {
  (void)lp;
  if (GetParent(hwnd)) return TRUE;            // skip owned/child windows
  if (!IsWindowVisible(hwnd)) return TRUE;
  WCHAR cls[64];
  if (!GetClassNameW(hwnd, cls, sizeof(cls) / sizeof(cls[0]))) return TRUE;
  if (wcmpI(cls, L"Chrome_WidgetWin_1") != 0) return TRUE;  // browser frames only
  // Scope to this install so other portable Chromes are never touched.
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return TRUE;
  WCHAR exe[MAX_PATH] = {0};
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h) {
    DWORD n = MAX_PATH;
    QueryFullProcessImageNameW(h, 0, exe, &n);
    CloseHandle(h);
  }
  WCHAR want[MAX_PATH];
  pjoin(want, MAX_PATH, g_app_dir, L"chrome.exe");
  if (wcmpI(exe, want) != 0) return TRUE;
  g_found = 1;
  // A window in a bad state — minimized, or mostly off-screen — is simply
  // maximized so it's always fully visible and usable after an update. Windows
  // that are already on-screen and normal are left untouched.
  if (IsIconic(hwnd)) {
    ShowWindow(hwnd, SW_MAXIMIZE);
    g_moved = 1;
    return TRUE;
  }
  RECT r; GetWindowRect(hwnd, &r);
  int w = r.right - r.left, hgt = r.bottom - r.top;
  if (w <= 0 || hgt <= 0) return TRUE;
  // Visible area of the window within the virtual screen.
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  int ix = (r.right < vx + vw ? r.right : vx + vw) - (r.left > vx ? r.left : vx);
  int iy = (r.bottom < vy + vh ? r.bottom : vy + vh) - (r.top > vy ? r.top : vy);
  if (ix < 0) ix = 0;
  if (iy < 0) iy = 0;
  long long vis = (long long)ix * iy;
  long long tot = (long long)w * hgt;
  // Mostly hidden (< 30% visible) or entirely off-screen → maximize it.
  if (tot > 0 && vis * 100 < tot * 30) {
    ShowWindow(hwnd, SW_MAXIMIZE);
    g_moved = 1;
  }
  return TRUE;
}

static void ensure_chrome_visible(void) {
  // Wait until the browser window exists (or ~7.5s timeout). The off-screen /
  // minimized correction (maximize) is applied synchronously inside
  // EnsureVisibleProc on the same pass it is found, so once a window is found
  // we can break immediately — there is no need to keep this GUI process alive
  // any longer. Keeping it alive longer only prolongs the image lock on our own
  // exe, which delays cleanup of updates\chrome_green_updater.exe.
  for (int t = 0; t < 25; t++) {
    g_found = 0; g_moved = 0;
    EnumWindows(EnsureVisibleProc, 0);
    if (g_found > 0) break;  // window found (and corrected if needed)
    Sleep(300);
  }
}

// ---- status reporting ----
static void set_status(int pct, const WCHAR* txt) {
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  g_percent = pct;
  wcpy(g_status, txt);
  if (g_hwnd && pct != g_last_post) {
    g_last_post = pct;
    PostMessageW(g_hwnd, WM_APP_STEP, 0, 0);
  }
}

// ---- worker thread ----
static DWORD WINAPI Worker(LPVOID lp) {
  (void)lp;
  SetCurrentDirectoryW(g_temp); // release any CWD lock on the install tree

  set_status(2, L"正在等待 Chrome 退出…");
  wait_pid_exit(g_pid, 10);
  if (g_abort) goto finish;

  set_status(8, L"正在等待进程释放…");
  wait_our_exit(15);
  if (g_abort) goto finish;

  set_status(14, L"正在关闭残留进程…");
  kill_pid(g_pid);
  kill_our();
  if (g_abort) goto finish;

  set_status(18, L"正在清理更新状态…");
  DeleteFileW(g_state_file);
  if (g_abort) goto finish;

  set_status(22, L"正在应用 chrome.exe 替换…");
  swap_chrome_new();
  if (g_abort) goto finish;

  set_status(26, L"正在清理残留目录…");
  sweep_orphans();
  if (g_abort) goto finish;

  WCHAR chrome_bin[600];
  pjoin(chrome_bin, 600, g_self_dll_dir, L"update_temp\\Chrome-bin");
  if (dir_exists(chrome_bin)) {
    set_status(30, L"正在复制 Chrome 文件…");
    g_total = count_bytes(chrome_bin); if (g_total <= 0) g_total = 1;
    g_copied_base = 0;
    copy_tree(chrome_bin, g_app_dir);
    rmtree(chrome_bin);
    set_status(78, L"正在处理旧版本目录…");
    handle_old_versions();
  } else {
    set_status(30, L"跳过：未找到 Chrome-bin（无需复制）");
  }
  if (g_abort) goto finish;

  if (g_self_update_ready && g_self_dll_new[0]) {
    set_status(85, L"正在更新 version.dll…");
    WCHAR old_dll[600];
    pjoin(old_dll, 600, g_self_dll_dir, L"version.dll");
    SetFileAttributesW(old_dll, FILE_ATTRIBUTE_NORMAL);
    CopyFileExW(g_self_dll_new, old_dll, NULL, NULL, NULL, 0);
    DeleteFileW(g_self_dll_new);
  } else {
    set_status(85, L"无需自更新 version.dll");
  }
  if (g_abort) goto finish;

  set_status(92, L"正在清理临时文件…");
  WCHAR ut[600];
  pjoin(ut, 600, g_self_dll_dir, L"update_temp");
  rmtree(ut);
  if (g_abort) goto finish;

  set_status(98, L"正在启动 Chrome…");
  launch_chrome(1);

finish:
  if (!g_abort) set_status(100, L"更新完成");
  else set_status(g_percent, L"已取消，正在退出…");
  // In real update mode (not preview, not aborted), remove the unguarded
  // manifest immediately. The running exe is image-locked and cannot be
  // deleted while this process is alive — both DeleteFileW and the
  // SetFileInformationByHandle/FileDispositionInfo trick fail with
  // ACCESS_DENIED on a live image. The exe is therefore removed by the next
  // Chrome run's startup cleanup (updater.cc InitUpdater), which retries the
  // delete once this GUI process has exited and released the image lock.
  if (!g_abort && !g_test_mode) {
    DeleteFileW(g_manifest);
  }
  if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
  return 0;
}

// ---- GDI+ (flat API, no C++ wrapper / no CRT) ----
// We forward-declare only the flat Gdip* functions we use. GDI+ objects are
// allocated inside gdiplus.dll, so no C runtime is needed on our side.
typedef int GpStatus;
typedef unsigned int ARGB;
typedef float REAL;
typedef struct GpGraphics GpGraphics;
typedef struct GpPen GpPen;
typedef struct GpBrush GpBrush;
typedef struct GpSolidFill GpSolidFill;
typedef struct GpPath GpPath;
typedef struct GpFontFamily GpFontFamily;
typedef struct GpFont GpFont;
typedef struct GpStringFormat GpStringFormat;
typedef struct GpBitmap GpBitmap;

#define SmoothingModeAntiAlias     4
#define TextRenderingHintClearTypeGridFit 3
#define FillModeWinding            0
#define MatrixOrderPrepend         0
#define UnitPixel                  2
#define StringAlignmentNear        0
#define StringAlignmentCenter      1
#define CombineModeIntersect       1
#define PixelFormat32bppARGB       0x0026200A
#define ImageLockModeRead          1

typedef struct { REAL X; REAL Y; REAL Width; REAL Height; } RectF;
typedef struct { int X; int Y; int Width; int Height; } Rect;
typedef struct { UINT Width; UINT Height; INT Stride; UINT PixelFormat; void* Scan0; UINT_PTR Reserved; } BitmapData;
typedef struct { UINT32 GdiplusVersion; void* DebugEventCallback; int SuppressBackgroundThread; int SuppressExternalCodecs; } GdiplusStartupInput;

GpStatus __stdcall GdipCreateBitmapFromScan0(INT, INT, INT, UINT, BYTE*, GpBitmap**);
GpStatus __stdcall GdipGetImageGraphicsContext(void*, GpGraphics**);
GpStatus __stdcall GdipSetSmoothingMode(GpGraphics*, int);
GpStatus __stdcall GdipSetTextRenderingHint(GpGraphics*, int);
GpStatus __stdcall GdipScaleWorldTransform(GpGraphics*, REAL, REAL, int);
GpStatus __stdcall GdipGraphicsClear(GpGraphics*, ARGB);
GpStatus __stdcall GdipCreatePath(int, GpPath**);
GpStatus __stdcall GdipStartPathFigure(GpPath*);
GpStatus __stdcall GdipAddPathArc(GpPath*, REAL, REAL, REAL, REAL, REAL, REAL);
GpStatus __stdcall GdipClosePathFigure(GpPath*);
GpStatus __stdcall GdipCreateSolidFill(ARGB, GpSolidFill**);
GpStatus __stdcall GdipFillPath(GpGraphics*, GpBrush*, GpPath*);
GpStatus __stdcall GdipCreatePen1(ARGB, REAL, int, GpPen**);
GpStatus __stdcall GdipDrawPath(GpGraphics*, GpPen*, GpPath*);
GpStatus __stdcall GdipCreateFontFamilyFromName(const WCHAR*, void*, GpFontFamily**);
GpStatus __stdcall GdipCreateFont(GpFontFamily*, REAL, INT, int, GpFont**);
GpStatus __stdcall GdipCreateStringFormat(INT, LANGID, GpStringFormat**);
GpStatus __stdcall GdipSetStringFormatAlign(GpStringFormat*, int);
GpStatus __stdcall GdipSetStringFormatLineAlign(GpStringFormat*, int);
GpStatus __stdcall GdipDeleteStringFormat(GpStringFormat*);
GpStatus __stdcall GdipDrawString(GpGraphics*, const WCHAR*, INT, GpFont*, const RectF*, GpStringFormat*, GpBrush*);
GpStatus __stdcall GdipSetClipRect(GpGraphics*, REAL, REAL, REAL, REAL, int);
GpStatus __stdcall GdipResetClip(GpGraphics*);
GpStatus __stdcall GdipDrawLine(GpGraphics*, GpPen*, REAL, REAL, REAL, REAL);
GpStatus __stdcall GdipBitmapLockBits(GpBitmap*, const Rect*, UINT, UINT, BitmapData*);
GpStatus __stdcall GdipBitmapUnlockBits(GpBitmap*, BitmapData*);
GpStatus __stdcall GdipDisposeImage(void*);
GpStatus __stdcall GdipDeleteGraphics(GpGraphics*);
GpStatus __stdcall GdipDeleteBrush(GpBrush*);
GpStatus __stdcall GdipDeletePen(GpPen*);
GpStatus __stdcall GdipDeletePath(GpPath*);
GpStatus __stdcall GdipDeleteFont(GpFont*);
GpStatus __stdcall GdipDeleteFontFamily(GpFontFamily*);
GpStatus __stdcall GdiplusStartup(ULONG_PTR*, const GdiplusStartupInput*, void*);
void     __stdcall GdiplusShutdown(ULONG_PTR);

// ---- layered-window state ----
static ULONG_PTR g_gdip = 0;
static HBITMAP g_hbm = NULL;
static HDC g_memdc = NULL;
static BYTE* g_bits = NULL;
static int g_w_dib = 0, g_h_dib = 0;
static float g_scale = 1.0f;
// 1 = use a layered window (smooth 8-bit alpha edges, scheme A).
// 0 = fall back to a plain GDI window (BitBlt of the same GDI+ bitmap) so the
//     window is ALWAYS visible even if UpdateLayeredWindow is unavailable.
// Default is 1 (scheme A): the earlier "invisible window" bug was caused by a
// mis-defined PixelFormat32bppARGB constant, NOT by the layered approach. Once
// that constant was corrected the offscreen bitmap is opaque (alpha=255) and
// the layered window composites correctly. --plain forces the safe fallback.
static int g_layered = 1;
// Standalone preview mode: launched with --test/--demo, or manually without a
// manifest (e.g. double-click). Shows the window and plays a simulated
// progress animation WITHOUT running the real, slow update sequence.
// (g_test_mode is declared near g_module/g_manifest at the top of the file.)
static int g_demo_pct = 0;

static ARGB argbf(COLORREF c) {
  return (ARGB)(0xFF000000UL |
         ((unsigned long)(GetRValue(c)) << 16) |
         ((unsigned long)(GetGValue(c)) << 8)  |
         ((unsigned long)(GetBValue(c))));
}

static void make_dpi_aware(void) {
  // PROCESS_PER_MONITOR_DPI_AWARE = 2 (shcore.dll). Fall back to the older
  // user32 SetProcessDPIAware if shcore is unavailable.
  int ok = 0;
  HMODULE sh = LoadLibraryW(L"shcore.dll");
  if (sh) {
    typedef HRESULT (WINAPI *SPF)(int);
    SPF f = (SPF)GetProcAddress(sh, "SetProcessDpiAwareness");
    if (f) { HRESULT hr = f(2); if (hr == 0 || hr == 1) ok = 1; }
    FreeLibrary(sh);
  }
  if (!ok) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
      typedef BOOL (WINAPI *SPDA)(void);
      SPDA f = (SPDA)GetProcAddress(u, "SetProcessDPIAware");
      if (f && f()) ok = 1;
    }
  }
}

static int create_dib(int w, int h) {
  BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  HDC scr = GetDC(NULL);
  g_hbm = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, (void**)&g_bits, NULL, 0);
  ReleaseDC(NULL, scr);
  if (!g_hbm) return 0;
  g_memdc = CreateCompatibleDC(NULL);
  SelectObject(g_memdc, g_hbm);
  g_w_dib = w; g_h_dib = h;
  return 1;
}

// Render the whole window into a GDI+ ARGB bitmap at physical resolution
// (scaled by DPI), then copy it (swapping R/B) into our DIB for the layered
// window. Anti-aliasing is done by GDI+, so the rounded corners are smooth.
//
// The effective scale is always g_w_dib / WIN_W so that content exactly fills
// the DIB regardless of how it was sized (DPI-scaled or fallback-corrected).
static void render(void) {
  if (g_w_dib <= 0 || g_h_dib <= 0) return;
  float eff_scale = (float)g_w_dib / (float)WIN_W; // dynamic: fills DIB exactly
  GpBitmap* bmp = NULL;
  if (GdipCreateBitmapFromScan0(g_w_dib, g_h_dib, 0, PixelFormat32bppARGB, NULL, &bmp) != 0) return;
  GpGraphics* g = NULL;
  if (GdipGetImageGraphicsContext(bmp, &g) != 0) { GdipDisposeImage(bmp); return; }

  GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
  GdipSetTextRenderingHint(g, TextRenderingHintClearTypeGridFit);
  GdipScaleWorldTransform(g, eff_scale, eff_scale, MatrixOrderPrepend);
  // In layered mode: clear to FULLY TRANSPARENT so that areas outside the
  // rounded-rect path become invisible (alpha=0).  The compositor then
  // clips the window to the smooth anti-aliased shape defined by our ARGB
  // bitmap — no SetWindowRgn needed.
  // In fallback (BitBlt) mode this would produce black corners, but that
  // path always calls draw_fallback() which paints a solid background first.
  GdipGraphicsClear(g, 0); // ARGB(0,0,0,0) = fully transparent

  // Rounded window body + 1px border (anti-aliased by GDI+).
  // d is the corner-ellipse diameter; matches the old CreateRoundRectRgn
  // (..., 18, 18) radius of 9 px so the look is unchanged, only smooth.
  GpPath* path = NULL; GdipCreatePath(FillModeWinding, &path);
  float d = 18.0f;
  GdipStartPathFigure(path);
  GdipAddPathArc(path, 0, 0, d, d, 180, 90);
  GdipAddPathArc(path, (REAL)(WIN_W - d), 0, d, d, 270, 90);
  GdipAddPathArc(path, (REAL)(WIN_W - d), (REAL)(WIN_H - d), d, d, 0, 90);
  GdipAddPathArc(path, 0, (REAL)(WIN_H - d), d, d, 90, 90);
  GdipClosePathFigure(path);

  // Fill the rounded path with opaque background — this makes the interior
  // solid while the exterior stays transparent (cleared above).
  GpSolidFill* bg = NULL; GdipCreateSolidFill(argbf(g_pal.bg), &bg);
  if (bg) GdipFillPath(g, (GpBrush*)bg, path);
  // No border stroke — a clean, borderless rounded window.

  // Title + status text.
  GpFontFamily* ff = NULL; GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &ff);
  GpFont* fTitle = NULL; if (ff) GdipCreateFont(ff, 16.0f, 1, UnitPixel, &fTitle);
  GpFont* fText  = NULL; if (ff) GdipCreateFont(ff, 13.0f, 0, UnitPixel, &fText);
  GpStringFormat* fmt = NULL; GdipCreateStringFormat(0, 0, &fmt);
  if (fmt) { GdipSetStringFormatAlign(fmt, StringAlignmentNear); GdipSetStringFormatLineAlign(fmt, StringAlignmentCenter); }

  GpSolidFill* titleBr = NULL; GdipCreateSolidFill(argbf(g_pal.title), &titleBr);
  RectF tr = { 16, 10, (REAL)(WIN_W - 40), 24 };
  if (fTitle && titleBr) GdipDrawString(g, L"Chrome Green · 正在更新", -1, fTitle, &tr, fmt, (GpBrush*)titleBr);

  GpSolidFill* stBr = NULL; GdipCreateSolidFill(argbf(g_pal.status), &stBr);
  RectF sr = { 22, 38, (REAL)(WIN_W - 44), 24 };
  if (fText && stBr) GdipDrawString(g, g_status, -1, fText, &sr, fmt, (GpBrush*)stBr);

  // Progress bar (rounded, clipped to current width).
  int bx = 22, by = 74, bw = WIN_W - 44, bh = 14, br = 7;
  int fw = bw * g_percent / 100;
  GpPath* bar = NULL; GdipCreatePath(FillModeWinding, &bar);
  float dd = (float)br; // corner-ellipse diameter = br (radius br/2), matches original
  GdipStartPathFigure(bar);
  GdipAddPathArc(bar, (REAL)bx, (REAL)by, dd, dd, 180, 90);
  GdipAddPathArc(bar, (REAL)(bx + bw - dd), (REAL)by, dd, dd, 270, 90);
  GdipAddPathArc(bar, (REAL)(bx + bw - dd), (REAL)(by + bh - dd), dd, dd, 0, 90);
  GdipAddPathArc(bar, (REAL)bx, (REAL)(by + bh - dd), dd, dd, 90, 90);
  GdipClosePathFigure(bar);

  GpSolidFill* trackBr = NULL; GdipCreateSolidFill(argbf(g_pal.track), &trackBr);
  if (trackBr) GdipFillPath(g, (GpBrush*)trackBr, bar);
  if (fw > 0 && bar) {
    GdipSetClipRect(g, (REAL)bx, (REAL)by, (REAL)fw, (REAL)bh, CombineModeIntersect);
    GpSolidFill* fillBr = NULL; GdipCreateSolidFill(argbf(g_pal.fill), &fillBr);
    if (fillBr) GdipFillPath(g, (GpBrush*)fillBr, bar);
    GdipResetClip(g);
    if (fillBr) GdipDeleteBrush((GpBrush*)fillBr);
  }

  // Close "x".
  GpPen* xp = NULL; GdipCreatePen1(argbf(g_pal.muted), 2.0f, UnitPixel, &xp);
  if (xp) {
    int cxs = WIN_W - 24, cys = 20;
    GdipDrawLine(g, xp, (REAL)(cxs - 5), (REAL)(cys - 5), (REAL)(cxs + 5), (REAL)(cys + 5));
    GdipDrawLine(g, xp, (REAL)(cxs + 5), (REAL)(cys - 5), (REAL)(cxs - 5), (REAL)(cys + 5));
  }

  // GDI+ gives BGRA (B,G,R,A) already; the DIB wants BGRA too. No channel
  // swap — doing one inverts red/blue (blue progress bar would show orange,
  // dark-blue bg would show brown). GDI+ returns STRAIGHT alpha, but
  // UpdateLayeredWindow expects PREMULTIPLIED, so premultiply here so the
  // anti-aliased rounded corners composite cleanly instead of leaving a halo.
  BitmapData bd; memset(&bd, 0, sizeof(bd));
  Rect lr = { 0, 0, g_w_dib, g_h_dib };
  if (GdipBitmapLockBits(bmp, &lr, ImageLockModeRead, PixelFormat32bppARGB, &bd) == 0) {
    BYTE* src = (BYTE*)bd.Scan0;
    int stride = bd.Stride;
    int row = g_w_dib * 4;
    for (int y = 0; y < g_h_dib; y++) {
      BYTE* s = src + y * stride;
      BYTE* d = g_bits + y * row;
      for (int x = 0; x < g_w_dib; x++) {
        BYTE a = s[3];
        d[0] = (BYTE)((s[0] * a) / 255); // B
        d[1] = (BYTE)((s[1] * a) / 255); // G
        d[2] = (BYTE)((s[2] * a) / 255); // R
        d[3] = a;                        // A
        d += 4; s += 4;
      }
    }
    GdipBitmapUnlockBits(bmp, &bd);
  }

  if (xp) GdipDeletePen(xp);
  if (bar) GdipDeletePath(bar);
  if (trackBr) GdipDeleteBrush((GpBrush*)trackBr);
  if (stBr) GdipDeleteBrush((GpBrush*)stBr);
  if (titleBr) GdipDeleteBrush((GpBrush*)titleBr);
  if (fmt) GdipDeleteStringFormat(fmt);
  if (fText) GdipDeleteFont(fText);
  if (fTitle) GdipDeleteFont(fTitle);
  if (ff) GdipDeleteFontFamily(ff);
  if (bg) GdipDeleteBrush((GpBrush*)bg);
  if (path) GdipDeletePath(path);
  GdipDeleteGraphics(g);
  GdipDisposeImage(bmp);
}

static int present(void) {
  if (!g_hwnd || !g_bits) return 0;
  render();
  RECT wr; GetWindowRect(g_hwnd, &wr);
  POINT ptDst = { wr.left, wr.top };
  SIZE sz = { g_w_dib, g_h_dib };
  POINT ptSrc = { 0, 0 };
  COLORREF ck = 0;
  BLENDFUNCTION bf;
  bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
  if (UpdateLayeredWindow(g_hwnd, NULL, &ptDst, &sz, g_memdc, &ptSrc, ck, &bf, ULW_ALPHA)) return 1;
  return 0;
}

// Fallback painting for the non-layered window: the same GDI+ bitmap is
// blitted onto the window DC. The rounded outline is provided by SetWindowRgn
// (1-bit corners, identical to the pre-GDI+ build) so the window is visible
// even when UpdateLayeredWindow is unavailable; the inner text/progress/border
// stay smooth because they come from GDI+.
//
// We paint a solid background FIRST so the window is never transparent/invisible
// even if render() (GDI+) silently fails and g_bits is empty on some machines.
static void draw_fallback(HDC hdc) {
  HBRUSH b = CreateSolidBrush(g_pal.bg);
  RECT rc = { 0, 0, g_w_dib, g_h_dib };
  FillRect(hdc, &rc, b);
  DeleteObject(b);
  if (g_bits) {
    render();
    BitBlt(hdc, 0, 0, g_w_dib, g_h_dib, g_memdc, 0, 0, SRCCOPY);
  }
}

// ---- UI ----
static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_APP_STEP) {
    if (g_layered) present(); else InvalidateRect(hw, NULL, FALSE);
    return 0;
  }
  if (msg == WM_APP_DONE) {
    if (g_layered) present(); else InvalidateRect(hw, NULL, FALSE);
    SetTimer(hw, 1, 700, NULL);
    return 0;
  }
  if (msg == WM_PAINT && !g_layered) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hw, &ps);
    draw_fallback(hdc);
    EndPaint(hw, &ps);
    return 0;
  }
  if (msg == WM_TIMER) {
    if (wp == 2 && g_test_mode) {
      // Simulated progress so the window can be inspected without updating.
      g_demo_pct += 3;
      if (g_demo_pct > 100) g_demo_pct = 0;
      WCHAR buf[64];
      wcpy(buf, L"测试模式 · ");
      WCHAR num[12]; ultow((unsigned)g_demo_pct, num); wcat(num, L"%");
      wcat(buf, num);
      g_percent = g_demo_pct;
      wcpy(g_status, buf);
      if (g_layered) present(); else InvalidateRect(hw, NULL, FALSE);
      SetTimer(hw, 2, 80, NULL);
      return 0;
    }
    KillTimer(hw, 1);
    DestroyWindow(hw);
    return 0;
  }
  if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
  if (msg == WM_CLOSE) {
    g_abort = 1;
    launch_chrome(0);
    ExitProcess(0);
    return 0;
  }
  // Make the whole window draggable (borderless). The close button area
  // returns HTCLOSE so the system draws the right cursor and routes the
  // click to WM_CLOSE; everywhere else returns HTCAPTION so a click-drag
  // moves the window.
  if (msg == WM_NCHITTEST) {
    POINT s; s.x = (short)LOWORD(lp); s.y = (short)HIWORD(lp);
    RECT wr; GetWindowRect(hw, &wr);
    int x = s.x - wr.left, y = s.y - wr.top;
    if (x >= g_closeRect.left && x <= g_closeRect.right &&
        y >= g_closeRect.top && y <= g_closeRect.bottom)
      return HTCLOSE;
    return HTCAPTION;
  }
  // WS_POPUP windows have no system menu, so returning HTCLOSE does NOT
  // auto-generate WM_CLOSE. Handle the click here so the [x] actually closes.
  if (msg == WM_NCLBUTTONDOWN) {
    if (wp == HTCLOSE) {
      g_abort = 1;
      launch_chrome(0);
      ExitProcess(0);
    }
    return 0;
  }
  // Explicitly control the cursor so we never fall back to an unexpected
  // system cursor (e.g. the I-beam) over the window. Use a hand over the
  // close button, the normal arrow everywhere else.
  if (msg == WM_SETCURSOR) {
    UINT ht = (UINT)((lp) & 0xFFFF);
    if (ht == HTCLOSE) SetCursor(LoadCursorW(NULL, IDC_HAND));
    else SetCursor(LoadCursorW(NULL, IDC_ARROW));
    return TRUE;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

static void place_centered(HWND hw) {
  int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
  int x = (cx - g_w_dib) / 2, y = (cy - g_h_dib) / 2;
  SetWindowPos(hw, NULL, x, y, g_w_dib, g_h_dib, SWP_NOZORDER);
  // Close button hit-rect: always proportional to DIB size so it stays
  // correct whether the DIB was sized by DPI or corrected by fallback.
  float eff_scale = (g_w_dib > 0) ? (float)g_w_dib / (float)WIN_W : 1.0f;
  g_closeRect.left   = (int)((WIN_W - 36) * eff_scale + 0.5f);
  g_closeRect.top    = (int)(8  * eff_scale + 0.5f);
  g_closeRect.right  = (int)((WIN_W - 12) * eff_scale + 0.5f);
  g_closeRect.bottom = (int)(32 * eff_scale + 0.5f);
}

static HWND create_gui() {
  WNDCLASSW wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"CGUpdaterW";
  // Without a class cursor a layered window shows the busy/wait cursor when
  // the mouse moves over it (DefWindowProc has nothing to restore). Set the
  // standard arrow so hovering behaves normally.
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = NULL; // both paths paint themselves
  if (!RegisterClassW(&wc)) return NULL;

  // Attempt 1: layered window with smooth 8-bit-alpha edges (scheme A).
  // Falls back to a plain GDI window (attempt 2) if UpdateLayeredWindow fails.
  HWND hw = NULL;
  if (g_layered) {
    hw = CreateWindowExW(WS_EX_LAYERED, L"CGUpdaterW", L"Chrome Green 更新",
                             WS_POPUP, CW_USEDEFAULT, 0, g_w_dib, g_h_dib,
                             NULL, NULL, wc.hInstance, NULL);
    if (hw) {
      g_hwnd = hw;
      place_centered(hw);
      // In layered mode the window shape is controlled by the per-pixel
      // alpha channel of our ARGB bitmap — NO SetWindowRgn needed.
      // SetWindowRgn would apply a 1-bit hard clip that destroys GDI+'s
      // smooth anti-aliased corners.  It is only used in fallback mode.

      // Set the bitmap while still hidden, then show — avoids a transparent flash.
      int ulw = present();
      if (ulw) {
        ShowWindow(hw, SW_SHOW);
        SetWindowPos(hw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        if (g_test_mode) SetTimer(hw, 2, 80, NULL);
        return hw;
      }
      // UpdateLayeredWindow failed: tear this down and fall back.
      DestroyWindow(hw);
      g_hwnd = NULL;
    } else {
      // CreateWindowEx(WS_EX_LAYERED) failed; fall through to plain window.
    }
  }

  // Attempt 2: plain GDI window. Always visible; inner content stays smooth
  // (GDI+ bitmap blitted in WM_PAINT), outer corner via SetWindowRgn.
  g_layered = 0;
  hw = CreateWindowExW(0, L"CGUpdaterW", L"Chrome Green 更新",
                       WS_POPUP, CW_USEDEFAULT, 0, g_w_dib, g_h_dib,
                       NULL, NULL, wc.hInstance, NULL);
  if (!hw) return NULL;
  g_hwnd = hw;

  // CRITICAL FIX: after creation, check actual client size vs DIB size.
  // If DPI awareness didn't take effect, the system may have scaled the
  // window to physical pixels while our DIB is only logical-sized → content
  // appears squished. Detect and recreate the DIB at the correct size.
  RECT ar; GetClientRect(hw, &ar);
  int aw = ar.right - ar.left, ah = ar.bottom - ar.top;
  if (aw > 0 && ah > 0 && (aw != g_w_dib || ah != g_h_dib)) {
    // Mismatch! Recreate DIB at the actual client size and re-render.
    if (g_bits) { DeleteObject(g_memdc); DeleteObject(g_hbm); g_bits = NULL; g_memdc = NULL; }
    g_w_dib = aw; g_h_dib = ah;
    create_dib(aw, ah);
  }

  place_centered(hw);
  int r = (int)(9 * g_scale + 0.5f);
  HRGN hr = CreateRoundRectRgn(0, 0, g_w_dib, g_h_dib, r, r);
  SetWindowRgn(hw, hr, TRUE);
  ShowWindow(hw, SW_SHOW);
  SetWindowPos(hw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  if (g_test_mode) SetTimer(hw, 2, 80, NULL);
  return hw;
}

// ---- manifest parsing ----
static void assign_val(const WCHAR* key, const WCHAR* val) {
  if (wcmpI(key, L"APP_DIR") == 0) wcpy(g_app_dir, val);
  else if (wcmpI(key, L"SELF_DLL_DIR") == 0) wcpy(g_self_dll_dir, val);
  else if (wcmpI(key, L"PID") == 0) g_pid = (DWORD)wtoul(val);
  else if (wcmpI(key, L"CHROME_EXE") == 0) wcpy(g_chrome_exe, val);
  else if (wcmpI(key, L"KEEP_OLD") == 0) g_keep_old = (wtoul(val) != 0);
  else if (wcmpI(key, L"KEEP_INSTALLER") == 0) g_keep_installer = (wtoul(val) != 0);
  else if (wcmpI(key, L"KEEP_VERSION") == 0) wcpy(g_keep_version, val);
  else if (wcmpI(key, L"SELF_UPDATE_READY") == 0) g_self_update_ready = (wtoul(val) != 0);
  else if (wcmpI(key, L"SELF_DLL_NEW") == 0) wcpy(g_self_dll_new, val);
  else if (wcmpI(key, L"STATE_FILE") == 0) wcpy(g_state_file, val);
  else if (wcmpI(key, L"THEME") == 0) wcpy(g_theme, val);
}
static void parse_manifest() {
  HANDLE h = CreateFileW(g_manifest, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) return;
  WCHAR buf[4096];
  DWORD n = 0;
  ReadFile(h, buf, sizeof(buf) - 2, &n, NULL);
  CloseHandle(h);
  buf[n / 2] = 0;
  WCHAR* p = buf;
  if (p[0] == 0xFEFF) p++; // skip UTF-16 BOM
  while (*p) {
    WCHAR* line = p;
    while (*p && *p != L'\n' && *p != L'\r') p++;
    WCHAR* end = p;
    while (*p == L'\r' || *p == L'\n') p++;
    *end = 0;
    WCHAR* eq = wchr(line, L'=');
    if (eq) { *eq = 0; assign_val(line, eq + 1); }
  }
}

// ---- minimal command-line parsing (no CRT) ----
static int cmd_has(const WCHAR* cmd, const WCHAR* flag) {
  int fl = wlen(flag);
  const WCHAR* p = cmd;
  while (*p) {
    while (*p == L' ' || *p == L'\t') p++;
    if (wncmpI(p, flag, fl) == 0) {
      WCHAR c = p[fl];
      if (c == 0 || c == L' ' || c == L'\t' || c == L'=') return 1;
    }
    while (*p && *p != L' ' && *p != L'\t') p++;
  }
  return 0;
}
// Returns a pointer to the value token following --flag (after '=' or
// whitespace), or NULL if the flag has no value. The caller must copy only up
// to the next whitespace.
static const WCHAR* cmd_val(const WCHAR* cmd, const WCHAR* flag) {
  int fl = wlen(flag);
  const WCHAR* p = cmd;
  while (*p) {
    while (*p == L' ' || *p == L'\t') p++;
    if (wncmpI(p, flag, fl) == 0) {
      const WCHAR* v = p + fl;
      if (*v == L'=') return v + 1;
      if (*v == L' ' || *v == L'\t') { while (*v == L' ' || *v == L'\t') v++; return v; }
    }
    while (*p && *p != L' ' && *p != L'\t') p++;
  }
  return NULL;
}

// ---- entry ----
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow) {
  (void)hInst; (void)hPrev; (void)nShow;
  // wWinMain's lpCmdLine is frequently empty under clang-cl's CRT wiring, so
  // arguments never arrive. Always resolve the real command line ourselves.
  if (!cmd || !*cmd) {
    WCHAR* c = GetCommandLineW();
    if (*c == L'"') { c++; while (*c && *c != L'"') c++; if (*c == L'"') c++; }
    else { while (*c && *c != L' ' && *c != L'\t') c++; }
    cmd = c; // points at the separator; cmd_has skips leading spaces
  }
  make_dpi_aware();

  // Standalone preview mode: explicit --test/--demo, or a manual launch with
  // no manifest (e.g. double-clicking the exe). Skips the real update flow.
  if (cmd_has(cmd, L"--test") || cmd_has(cmd, L"--demo")) g_test_mode = 1;

  // Window style override. Default is scheme A (layered, smooth 8-bit corners).
  // --plain forces the safe plain-window fallback; --layered is the default.
  if (cmd_has(cmd, L"--plain")) g_layered = 0;
  else if (cmd_has(cmd, L"--layered")) g_layered = 1;

  // GDI+ init.
  GdiplusStartupInput si; memset(&si, 0, sizeof(si));
  si.GdiplusVersion = 1;
  if (GdiplusStartup(&g_gdip, &si, NULL) != 0) g_gdip = 0;

  // Physical size derived from the primary monitor DPI.
  HDC ddc = GetDC(NULL); int dpi = GetDeviceCaps(ddc, LOGPIXELSX); ReleaseDC(NULL, ddc);
  if (dpi <= 0) dpi = 96;
  g_scale = (float)dpi / 96.0f;
  g_w_dib = (int)(WIN_W * g_scale + 0.5f);
  g_h_dib = (int)(WIN_H * g_scale + 0.5f);
  int dib_ok = create_dib(g_w_dib, g_h_dib);
  if (g_gdip == 0 || !dib_ok) {
    if (g_test_mode) {
      // Nothing to draw without GDI+/offscreen bitmap.
      if (g_gdip) GdiplusShutdown(g_gdip);
      ExitProcess(0);
      return 0;
    }
    // No GDI+ / no offscreen bitmap: run the update synchronously so it is
    // never lost even if the layered window cannot be created.
    if (g_bits) { DeleteObject(g_memdc); DeleteObject(g_hbm); }
    Worker(NULL);
    if (g_gdip) GdiplusShutdown(g_gdip);
    ExitProcess(0);
    return 0;
  }

  GetModuleFileNameW(NULL, g_module, 520);
  WCHAR* sl = wchr(g_module, L'\\');
  WCHAR* last = sl;
  while (sl) { last = sl; sl = wchr(last + 1, L'\\'); }
  if (last) last[1] = 0; // truncate to directory
  wcpy(g_theme, L"auto"); // default until the manifest overrides it
  wcpy(g_manifest, g_module);
  wcat(g_manifest, L"chrome_green_updater_manifest.txt");
  if (!file_exists(g_manifest)) g_test_mode = 1; // manual launch => preview
  parse_manifest();
  GetTempPathW(520, g_temp);

  if (g_test_mode) {
    // Optional --theme override for previewing light/dark palettes.
    const WCHAR* tv = cmd_val(cmd, L"--theme");
    if (tv && *tv) {
      int i = 0;
      while (tv[i] && tv[i] != L' ' && tv[i] != L'\t' && i < 15) { g_theme[i] = tv[i]; i++; }
      g_theme[i] = 0;
    }
    wcpy(g_status, L"测试模式 · 窗口预览");
    g_percent = 0;
  } else {
    wcpy(g_status, L"正在准备更新…");
    g_percent = 0;
  }

  resolve_theme(); // apply light/dark palette matching the config page
  g_hwnd = create_gui();
  if (!g_hwnd) {
    // Headless fallback: run the update sequence synchronously so the update
    // is never lost even if the window cannot be created.
    if (g_test_mode) { if (g_gdip) GdiplusShutdown(g_gdip); ExitProcess(0); return 0; }
    Worker(NULL);
    if (g_gdip) GdiplusShutdown(g_gdip);
    ExitProcess(0);
    return 0;
  }
  if (!g_test_mode) CreateThread(NULL, 0, Worker, NULL, 0, NULL);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_gdip) GdiplusShutdown(g_gdip);
  if (g_bits) { DeleteObject(g_memdc); DeleteObject(g_hbm); }
  ExitProcess(0);
  return 0;
}
