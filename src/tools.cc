#include "tools.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <shobjidl.h>
#include <propkey.h>
#include <shlwapi.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "appid.h"
#include "com_initializer.h"
#include "utils.h"

namespace {

std::wstring GetOurChromeExe() {
  return GetAppDir() + L"\\chrome.exe";
}

// Normalize a path for case-insensitive comparison.
std::wstring Canonicalize(const std::wstring& path) {
  wchar_t buf[MAX_PATH] = {0};
  if (PathCanonicalizeW(buf, path.c_str())) return buf;
  return path;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(Canonicalize(a).c_str(), Canonicalize(b).c_str()) == 0;
}

// Resolve a .lnk to its target executable path.
bool ResolveLinkTarget(const std::wstring& lnk_path, std::wstring& out_target) {
  IShellLinkW* psl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW, reinterpret_cast<void**>(&psl))))
    return false;
  IPersistFile* ppf = nullptr;
  bool ok = false;
  if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile,
                                    reinterpret_cast<void**>(&ppf)))) {
    if (SUCCEEDED(ppf->Load(lnk_path.c_str(), STGM_READ))) {
      wchar_t buf[32768] = {0};
      if (SUCCEEDED(
              psl->GetPath(buf, _countof(buf), nullptr,
                           SLGP_RAWPATH | SLGP_UNCPRIORITY)) &&
          buf[0] != L'\0') {
        out_target = buf;
        ok = true;
      }
    }
    ppf->Release();
  }
  psl->Release();
  return ok;
}

// Enumerate *.lnk in a folder; invoke cb(lnk_path) for each.
void ForEachLink(const std::wstring& folder,
                 const std::function<void(const std::wstring&)>& cb) {
  WIN32_FIND_DATAW fd = {0};
  std::wstring pattern = folder + L"\\*.lnk";
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      cb(folder + L"\\" + fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// Resolve the default (non-portable) Chrome data directory:
//   C:\Users\<user>\AppData\Local\Google\Chrome
std::wstring GetDefaultChromeDataDir() {
  wchar_t* local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
    return {};
  std::wstring dir(local);
  CoTaskMemFree(local);
  return dir + L"\\Google\\Chrome";
}

// Recursively delete a directory and all of its contents (Win32, no CRT).
void RemoveDirRecursive(const std::wstring& dir) {
  std::wstring search = dir + L"\\*";
  WIN32_FIND_DATAW fd = {0};
  HANDLE h = FindFirstFileW(search.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    RemoveDirectoryW(dir.c_str());
    return;
  }
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring path = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      RemoveDirRecursive(path);
    else {
      SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(path.c_str());
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  RemoveDirectoryW(dir.c_str());
}

}  // namespace

std::string ToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                              nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}

ToolsResult CreateDesktopShortcut() {
  ComInitializer com;
  if (!com.IsInitialized())
    return {false, L"COM initialization failed", false};

  wchar_t* desktop = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
    return {false, L"Failed to resolve Desktop folder", false};
  std::wstring desktop_dir(desktop);
  CoTaskMemFree(desktop);

  std::wstring app_dir = GetAppDir();
  std::wstring chrome_exe = GetOurChromeExe();

  // Pick "Google Chrome.lnk", then "Google Chrome (2).lnk", ... An existing
  // "Google Chrome.lnk" that already targets our chrome.exe is reused.
  std::wstring base = desktop_dir + L"\\Google Chrome";
  std::wstring link_path;
  int index = 0;
  for (;;) {
    std::wstring name =
        base +
        (index == 0 ? L"" : L" (" + std::to_wstring(index) + L")") +
        L".lnk";
    if (!PathFileExistsW(name.c_str())) {
      link_path = name;
      break;
    }
    if (index == 0) {
      std::wstring tgt;
      if (ResolveLinkTarget(name, tgt) && SamePath(tgt, chrome_exe)) {
        link_path = name;
        break;
      }
    }
    if (++index > 999) return {false, L"Too many existing shortcuts", false};
  }

  IShellLinkW* psl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW, reinterpret_cast<void**>(&psl))))
    return {false, L"Failed to create shell link", false};

  psl->SetPath(chrome_exe.c_str());
  psl->SetWorkingDirectory(app_dir.c_str());
  psl->SetDescription(L"");
  psl->SetIconLocation(chrome_exe.c_str(), 0);
  psl->SetArguments(L"--portable");

  IPersistFile* ppf = nullptr;
  bool saved = false;
  if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile,
                                    reinterpret_cast<void**>(&ppf)))) {
    saved = SUCCEEDED(ppf->Save(link_path.c_str(), TRUE));
    ppf->Release();
  }
  psl->Release();

  if (!saved) return {false, L"Failed to save desktop shortcut", false};
  DebugLog(L"Created desktop shortcut: {}", link_path);
  return {true, L"Desktop shortcut created", true};
}

bool DesktopShortcutExists() {
  ComInitializer com;
  if (!com.IsInitialized()) return false;
  wchar_t* desktop = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
    return false;
  std::wstring desktop_dir(desktop);
  CoTaskMemFree(desktop);
  std::wstring our_exe = GetOurChromeExe();
  bool exists = false;
  ForEachLink(desktop_dir, [&](const std::wstring& lnk) {
    if (exists) return;
    std::wstring target;
    if (ResolveLinkTarget(lnk, target) && SamePath(target, our_exe))
      exists = true;
  });
  return exists;
}

ChromeDataStatus GetChromeDefaultDataStatus() {
  ChromeDataStatus s;
  std::wstring dir = GetDefaultChromeDataDir();
  if (dir.empty()) return s;
  if (!PathFileExistsW(dir.c_str())) {
    s.exists = false;
    s.empty = true;
    return s;
  }
  s.exists = true;
  std::wstring search = dir + L"\\*";
  WIN32_FIND_DATAW fd = {0};
  HANDLE h = FindFirstFileW(search.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    s.empty = true;
    return s;
  }
  int count = 0;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    ++count;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  s.entry_count = count;
  s.empty = (count == 0);
  return s;
}

ToolsResult CleanChromeDefaultData() {
  std::wstring dir = GetDefaultChromeDataDir();
  if (dir.empty())
    return {false, L"Failed to resolve Local AppData folder", false};

  // Safety guard: only the exact "...\Google\Chrome" path may ever be removed.
  const wchar_t* suffix = L"\\Google\\Chrome";
  if (dir.length() < wcslen(suffix) ||
      _wcsicmp(dir.c_str() + dir.length() - wcslen(suffix), suffix) != 0)
    return {false, L"Unexpected path; aborting for safety", false};

  if (!PathFileExistsW(dir.c_str()))
    return {true, L"Already clean (directory does not exist)", true};

  RemoveDirRecursive(dir);
  if (PathFileExistsW(dir.c_str()))
    return {false, L"Failed to remove directory", false};

  DebugLog(L"Cleaned default Chrome data dir: {}", dir);
  return {true, L"Default Chrome data directory cleaned", true};
}
