#include "appid.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

#include <propkey.h>
#include <shobjidl.h>

#include <memory>
#include <string>

#include "detours.h"

#include "diaglog.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// Strategy: FORCE MODE — all AppUserModelID calls are redirected to our own
// install-unique id "ChromeGreen.<hash>" (computed from the AppDir). This makes
// the running windows AND any pinned taskbar shortcut share ONE identity, so
// the taskbar icon groups (the classic "pinned chrome.exe won't consolidate"
// bug). The jump-list menu is intentionally left to Chrome's default (no custom
// tasks) to avoid the previous crash/redirect-to-wrong-browser problems.
// A matching icon is registered under HKCU\Software\Classes\AppUserModelID so
// the grouped icon is not blank.
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

std::wstring ToHex8(uint32_t v) {
  wchar_t buf[16] = {0};
  swprintf(buf, 16, L"%08X", v);
  return buf;
}

}  // namespace

std::wstring GetGreenAumid() {
  std::wstring dir = GetAppDir();
  std::wstring id = L"ChromeGreen." + ToHex8(Fnv1a32(dir));
  return id;
}

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

// ---------------------------------------------------------------------------
// Layer 1: process-level AppUserModelID — PASS-THROUGH
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* FnSetCurrentProcessExplicitAppUserModelID)(PCWSTR);
static FnSetCurrentProcessExplicitAppUserModelID
    RealSetCurrentProcessExplicitAppUserModelID = nullptr;

static HRESULT WINAPI MySetCurrentProcessExplicitAppUserModelID(PCWSTR pszAppID) {
  // FORCE: redirect the process AUMID to our install-unique id regardless of
  // what Chrome requests. This is what makes the running process group with a
  // pinned shortcut that carries the same AUMID.
  static const std::wstring ours = GetGreenAumid();
  DiagLog(L"[APPID] SetCurrentProcessExplicitAppUserModelID requested \"{}\" "
          L"-> forcing \"{}\"",
          pszAppID ? pszAppID : L"(null)", ours);
  return RealSetCurrentProcessExplicitAppUserModelID(ours.c_str());
}

// ---------------------------------------------------------------------------
// Layer 2: per-window AppUserModelID — PASS-THROUGH
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* FnSHSetTemporaryPropertyForWindow)(HWND, REFPROPERTYKEY,
                                                          REFPROPVARIANT);
static FnSHSetTemporaryPropertyForWindow RealSHSetTemporaryPropertyForWindow =
    nullptr;

static HRESULT WINAPI MySHSetTemporaryPropertyForWindow(HWND hwnd,
                                                       REFPROPERTYKEY key,
                                                       REFPROPVARIANT propvar) {
  if (key.fmtid == PKEY_AppUserModel_ID.fmtid &&
      key.pid == PKEY_AppUserModel_ID.pid && propvar.vt == VT_LPWSTR &&
      propvar.pwszVal && propvar.pwszVal[0] != L'\0') {
    // FORCE: override the per-window AUMID Chrome tries to set (e.g. to
    // separate incognito / app windows) with our install-unique id so every
    // window groups under the same taskbar button.
    static const std::wstring ours = GetGreenAumid();
    DiagLog(L"[APPID] SHSetTemporaryPropertyForWindow AUMID \"{}\" -> \"{}\"",
            propvar.pwszVal, ours);
    PROPVARIANT pv = propvar;
    pv.pwszVal = const_cast<wchar_t*>(ours.c_str());
    return RealSHSetTemporaryPropertyForWindow(hwnd, key, pv);
  }
  return RealSHSetTemporaryPropertyForWindow(hwnd, key, propvar);
}

// ---------------------------------------------------------------------------
// Layer 2b: per-window AppUserModelID via SHGetPropertyStoreForWindow
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* FnSHGetPropertyStoreForWindow)(HWND, REFIID, void**);
static FnSHGetPropertyStoreForWindow RealSHGetPropertyStoreForWindow = nullptr;

class PropertyStoreWrapper : public IPropertyStore {
 public:
  explicit PropertyStoreWrapper(IPropertyStore* inner) : inner_(inner) {
    inner_->AddRef();
  }
  virtual ~PropertyStoreWrapper() { inner_->Release(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IPropertyStore || riid == IID_IUnknown) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return inner_->QueryInterface(riid, ppv);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }

  HRESULT STDMETHODCALLTYPE GetCount(DWORD* c) override {
    return inner_->GetCount(c);
  }
  HRESULT STDMETHODCALLTYPE GetAt(DWORD i, PROPERTYKEY* k) override {
    return inner_->GetAt(i, k);
  }
  HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY k,
                                     PROPVARIANT* v) override {
    return inner_->GetValue(k, v);
  }
  HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY k,
                                     REFPROPVARIANT v) override {
    if (k.fmtid == PKEY_AppUserModel_ID.fmtid &&
        k.pid == PKEY_AppUserModel_ID.pid && v.vt == VT_LPWSTR && v.pwszVal &&
        v.pwszVal[0] != L'\0') {
      // FORCE: same as the SHSetTemporaryPropertyForWindow path — redirect any
      // per-window AUMID Chrome writes through the property store.
      static const std::wstring ours = GetGreenAumid();
      DiagLog(L"[APPID] property-store window AUMID \"{}\" -> \"{}\"",
              v.pwszVal, ours);
      PROPVARIANT pv = v;
      pv.pwszVal = const_cast<wchar_t*>(ours.c_str());
      return inner_->SetValue(k, pv);
    }
    return inner_->SetValue(k, v);
  }
  HRESULT STDMETHODCALLTYPE Commit() override { return inner_->Commit(); }

 private:
  IPropertyStore* inner_;
  ULONG ref_ = 1;
};

static HRESULT WINAPI MySHGetPropertyStoreForWindow(HWND hwnd, REFIID riid,
                                                   void** ppv) {
  HRESULT hr = RealSHGetPropertyStoreForWindow(hwnd, riid, ppv);
  if (SUCCEEDED(hr) && ppv && *ppv && riid == IID_IPropertyStore) {
    IPropertyStore* real = static_cast<IPropertyStore*>(*ppv);
    *ppv = new PropertyStoreWrapper(real);
    real->Release();
  }
  return hr;
}

// ---------------------------------------------------------------------------
// Register an icon + display name for our AUMID under
// HKCU\Software\Classes\AppUserModelID\<AUMID> so the grouped taskbar button
// (which now carries our custom id) shows Chrome's icon instead of a blank
// document. Without this, a forced-custom AUMID renders as a generic/empty
// icon on the taskbar. Called once at startup; idempotent.
// ---------------------------------------------------------------------------
static void RegisterAumidIcon() {
  std::wstring aumid = GetGreenAumid();
  std::wstring key = L"Software\\Classes\\AppUserModelID\\" + aumid;
  HKEY hk = nullptr;
  LONG st = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                            &hk, nullptr);
  if (st != ERROR_SUCCESS) {
    DiagLog(L"[APPID] RegisterAumidIcon: RegCreateKeyEx failed {}",
            static_cast<int>(st));
    return;
  }
  std::wstring icon = GetAppDir() + L"\\chrome.exe,0";
  RegSetValueExW(hk, L"Icon", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(icon.c_str()),
                 static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));
  RegSetValueExW(hk, L"DisplayName", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(L"ChromeGreen"),
                 static_cast<DWORD>((wcslen(L"ChromeGreen") + 1) *
                                    sizeof(wchar_t)));
  RegCloseKey(hk);
  DiagLog(L"[APPID] Registered AUMID icon: {} -> {}", aumid, icon);
}

// ---------------------------------------------------------------------------
// Hook installation (idempotent)
// ---------------------------------------------------------------------------
static void InstallHooks() {
  static bool installed = false;
  if (installed) return;
  installed = true;

  HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
  if (!shell32) shell32 = LoadLibraryW(L"shell32.dll");

  RealSetCurrentProcessExplicitAppUserModelID =
      reinterpret_cast<FnSetCurrentProcessExplicitAppUserModelID>(
          GetProcAddress(shell32,
                         "SetCurrentProcessExplicitAppUserModelID"));
  RealSHSetTemporaryPropertyForWindow =
      reinterpret_cast<FnSHSetTemporaryPropertyForWindow>(
          GetProcAddress(shell32, "SHSetTemporaryPropertyForWindow"));
  RealSHGetPropertyStoreForWindow =
      reinterpret_cast<FnSHGetPropertyStoreForWindow>(
          GetProcAddress(shell32, "SHGetPropertyStoreForWindow"));

  DiagLog(L"[APPID] SetAppId: FORCE MODE — redirect process / window AUMID to "
          L"\"{}\", register icon, no jump-list hook",
          GetGreenAumid());

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  if (RealSetCurrentProcessExplicitAppUserModelID)
    DetourAttach(
        reinterpret_cast<PVOID*>(&RealSetCurrentProcessExplicitAppUserModelID),
        reinterpret_cast<PVOID>(MySetCurrentProcessExplicitAppUserModelID));
  if (RealSHSetTemporaryPropertyForWindow)
    DetourAttach(
        reinterpret_cast<PVOID*>(&RealSHSetTemporaryPropertyForWindow),
        reinterpret_cast<PVOID>(MySHSetTemporaryPropertyForWindow));
  if (RealSHGetPropertyStoreForWindow)
    DetourAttach(
        reinterpret_cast<PVOID*>(&RealSHGetPropertyStoreForWindow),
        reinterpret_cast<PVOID>(MySHGetPropertyStoreForWindow));
  DetourTransactionCommit();
}

void SetAppId() {
  RegisterAumidIcon();
  InstallHooks();
  // NOTE: We do NOT register a custom jump-list here. Chrome writes its own
  // jump-list (Recent, Tasks) under its *native* AUMID, not our forced
  // ChromeGreen.* id. Writing custom tasks under our id would show a fake menu
  // that doesn't match stock Chrome's and whose chrome:// URLs don't work as
  // command-line arguments. Let the taskbar show only the system-default items
  // (running windows list, pin/unpin, close window).
}
