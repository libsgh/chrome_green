#include "update.h"

#include <windows.h>
#include <winhttp.h>
#include <winver.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include "utils.h"
#include "version.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")

// Global state
UpdateStateData g_update_state;
std::mutex g_update_mutex;

// Debug log ring buffer
std::deque<std::string> g_debug_logs;
std::mutex g_debug_log_mutex;

// Gate for in-memory debug logging. When false, AddDebugLog() is a no-op so
// no logs are recorded. Set from config (g_enable_debug_log, defined here).
bool g_enable_debug_log = false;

void AddDebugLog(const std::string& msg) {
  if (!g_enable_debug_log) return;
  std::lock_guard<std::mutex> lock(g_debug_log_mutex);
  // Timestamp
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  char ts[64];
  struct tm tm_info;
  localtime_s(&tm_info, &t);
  strftime(ts, sizeof(ts), "%H:%M:%S", &tm_info);
  std::ostringstream ss;
  ss << "[" << ts << "." << std::setfill('0') << std::setw(3) << ms.count() << "] " << msg;
  g_debug_logs.push_back(ss.str());
  // Keep at most kMaxLogLines
  while (g_debug_logs.size() > kMaxLogLines) {
    g_debug_logs.pop_front();
  }
}

std::vector<std::string> GetDebugLogs() {
  std::lock_guard<std::mutex> lock(g_debug_log_mutex);
  return std::vector<std::string>(g_debug_logs.begin(), g_debug_logs.end());
}

void ClearDebugLogs() {
  std::lock_guard<std::mutex> lock(g_debug_log_mutex);
  g_debug_logs.clear();
}


namespace {

// --- Omaha XML request templates (Windows only) ---

// App IDs
constexpr const char* kAppIdStable = "{8A69D345-D564-463C-AFF1-A69D9E530F96}";
constexpr const char* kAppIdCanary = "{4EA16AC7-FD5A-47C3-875B-DBF4A2008C20}";

}  // namespace (pause for public symbols)

std::string WstrToUtf8(const std::wstring& wstr) {
  if (wstr.empty()) return "";
  int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                nullptr, 0, nullptr, nullptr);
  std::string result(len, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                      result.data(), len, nullptr, nullptr);
  return result;
}

std::wstring Utf8ToWstr(const std::string& str) {
  if (str.empty()) return L"";
  int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(),
                                nullptr, 0);
  std::wstring result(len, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(),
                      result.data(), len);
  return result;
}

// --- Minimal JSON/XML escaping ---

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }
  return out;
}

namespace {  // resume

// Build the Omaha XML request body for the given channel + arch.
std::string BuildOmahaRequest(UpdateChannel channel, UpdateArch arch,
                               const std::string& current_version) {
  const char* appid = GetAppId(channel);
  std::string ap = GetApParameter(channel, arch);

  std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<request protocol="3.0" updater="Omaha" updaterversion="1.3.36.152" shell_version="1.3.36.151" ismachine="0" sessionid="{11111111-1111-1111-1111-111111111111}" installsource="taggedmi" requestid="{11111111-1111-1111-1111-111111111111}" dedup="cr" domainjoined="0">
<hw physmemory="16" sse="1" sse2="1" sse3="1" ssse3="1" sse41="1" sse42="1" avx="1"/>
<os platform="win" version="10.0.22621.1028" sp="" arch=")";
  xml += ArchToString(arch);
  xml += R"("/>
<app appid=")";
  xml += appid;
  xml += R"(" version=")";
  xml += JsonEscape(current_version);
  xml += R"(" nextversion="" ap=")";
  xml += ap;
  xml += R"(" lang="en" brand="" client="" installage="-1" installdate="-1" iid="{11111111-1111-1111-1111-111111111111}">
	<updatecheck/>
	<data name="install" index="empty"/>
</app>
</request>)";
  return xml;
}

// --- Minimal XML string extraction ---
// The Omaha response is simple and predictable. We use string search rather
// than a full XML parser to avoid dependencies.

// Extract the content between <tag> and </tag> (first occurrence).
std::string ExtractTag(const std::string& xml, const std::string& tag) {
  std::string open = "<" + tag;
  std::string close = "</" + tag + ">";

  auto start = xml.find(open);
  if (start == std::string::npos) return "";

  // Skip attributes to find the '>' that closes the opening tag.
  auto tag_end = xml.find('>', start);
  if (tag_end == std::string::npos) return "";

  auto end = xml.find(close, tag_end + 1);
  if (end == std::string::npos) return "";

  return xml.substr(tag_end + 1, end - tag_end - 1);
}

// Extract an attribute value from an XML tag string.
// e.g. from `version="132.0.6834.59"` extract `132.0.6834.59`
std::string ExtractAttr(const std::string& s, const std::string& attr) {
  std::string pattern = attr + "=\"";
  auto start = s.find(pattern);
  if (start == std::string::npos) return "";
  start += pattern.length();
  auto end = s.find('"', start);
  if (end == std::string::npos) return "";
  return s.substr(start, end - start);
}

// --- System proxy detection ---

bool GetSystemProxy(std::wstring& proxy_server) {
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                    0, KEY_READ, &hKey) != ERROR_SUCCESS) {
    return false;
  }

  DWORD enabled = 0;
  DWORD size = sizeof(enabled);
  RegQueryValueExW(hKey, L"ProxyEnable", nullptr, nullptr,
                   reinterpret_cast<LPBYTE>(&enabled), &size);

  if (enabled) {
    wchar_t buf[256] = {0};
    size = sizeof(buf);
    RegQueryValueExW(hKey, L"ProxyServer", nullptr, nullptr,
                     reinterpret_cast<LPBYTE>(buf), &size);
    proxy_server = buf;
  }

  RegCloseKey(hKey);
  return enabled != 0;
}

}  // namespace

// --- Public API implementations ---

const char* GetAppId(UpdateChannel channel) {
  return channel == UpdateChannel::kCanary ? kAppIdCanary : kAppIdStable;
}

std::string GetApParameter(UpdateChannel channel, UpdateArch arch) {
  std::string ap;
  ap += ArchToString(arch);
  ap += "-";
  ap += ChannelToStringA(channel);
  ap += "-statsdef_1";
  return ap;
}

const wchar_t* ChannelToString(UpdateChannel channel) {
  switch (channel) {
    case UpdateChannel::kStable: return L"stable";
    case UpdateChannel::kBeta:   return L"beta";
    case UpdateChannel::kDev:    return L"dev";
    case UpdateChannel::kCanary: return L"canary";
  }
  return L"stable";
}

const char* ChannelToStringA(UpdateChannel channel) {
  switch (channel) {
    case UpdateChannel::kStable: return "stable";
    case UpdateChannel::kBeta:   return "beta";
    case UpdateChannel::kDev:    return "dev";
    case UpdateChannel::kCanary: return "canary";
  }
  return "stable";
}

const char* ArchToString(UpdateArch arch) {
  switch (arch) {
    case UpdateArch::kX64:   return "x64";
    case UpdateArch::kX86:   return "x86";
    case UpdateArch::kARM64: return "arm64";
  }
  return "x64";
}

UpdateChannel ParseChannel(const std::wstring& str) {
  if (str == L"beta")   return UpdateChannel::kBeta;
  if (str == L"dev")    return UpdateChannel::kDev;
  if (str == L"canary") return UpdateChannel::kCanary;
  return UpdateChannel::kStable;
}

std::string GetInstalledChromeVersion() {
  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

  DWORD dummy = 0;
  DWORD size = GetFileVersionInfoSizeW(exe_path, &dummy);
  if (size == 0) return "";

  std::vector<BYTE> buffer(size);
  if (!GetFileVersionInfoW(exe_path, 0, size, buffer.data())) return "";

  VS_FIXEDFILEINFO* ffi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buffer.data(), L"\\",
                      reinterpret_cast<LPVOID*>(&ffi), &len)) {
    return "";
  }
  if (len < sizeof(VS_FIXEDFILEINFO)) return "";

  // HIWORD.major LOWORD.minor HIWORD.build LOWORD.patch
  std::ostringstream ss;
  ss << HIWORD(ffi->dwFileVersionMS) << "."
     << LOWORD(ffi->dwFileVersionMS) << "."
     << HIWORD(ffi->dwFileVersionLS) << "."
     << LOWORD(ffi->dwFileVersionLS);
  return ss.str();
}

std::wstring GetUpdateStatePath() {
  // Place the state file inside Chrome's data directory (<DLL dir>\..\Data)
  // so it lives with the browser's data and keeps the app directory clean
  // (only chrome_green.ini + config live there).
  std::wstring data_dir = CanonicalizePath(GetSelfDllDir() + L"\\..\\Data");
  return data_dir + L"\\chrome_green_update.json";
}

// --- State load / save (minimal JSON) ---

void LoadUpdateState() {
  std::lock_guard<std::mutex> lock(g_update_mutex);

  std::wstring path = GetUpdateStatePath();
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return;

  std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

  if (content.empty()) return;

  // Minimal JSON parsing with string search
  auto find_str = [&](const std::string& key) -> std::string {
    std::string pattern = "\"" + key + "\":\"";
    auto s = content.find(pattern);
    if (s == std::string::npos) return "";
    s += pattern.length();
    auto e = content.find('"', s);
    if (e == std::string::npos) return "";
    return content.substr(s, e - s);
  };

  auto find_num = [&](const std::string& key) -> int64_t {
    std::string pattern = "\"" + key + "\":";
    auto s = content.find(pattern);
    if (s == std::string::npos) return 0;
    s += pattern.length();
    // Skip optional leading whitespace (defensive against hand-edited files).
    while (s < content.size() && (content[s] == ' ' || content[s] == '\t')) s++;
    // Only accept a leading sign/digit; bail to 0 otherwise so that a boolean
    // value (e.g. "true") or other non-numeric token can never throw.
    if (s >= content.size() ||
        !(content[s] == '-' ||
          (content[s] >= '0' && content[s] <= '9'))) {
      return 0;
    }
    auto e = s;
    while (e < content.size() &&
           (content[e] >= '0' && content[e] <= '9')) {
      e++;
    }
    std::string num = content.substr(s, e - s);
    errno = 0;
    char* endp = nullptr;
    long long v = std::strtoll(num.c_str(), &endp, 10);
    if (endp == num.c_str() || errno != 0) return 0;
    return static_cast<int64_t>(v);
  };

  // Safe 64-bit integer parse that never throws (the global exception policy
  // terminates on std::stoll failure). Used for the speed/eta fields.
  auto find_i64 = [&](const std::string& key, int64_t def) -> int64_t {
    std::string pattern = "\"" + key + "\":";
    auto s = content.find(pattern);
    if (s == std::string::npos) return def;
    s += pattern.length();
    auto e = s;
    while (e < content.size() &&
           (content[e] == '-' || (content[e] >= '0' && content[e] <= '9'))) {
      e++;
    }
    if (e == s) return def;
    std::string num = content.substr(s, e - s);
    errno = 0;
    char* endp = nullptr;
    long long v = std::strtoll(num.c_str(), &endp, 10);
    if (endp == num.c_str() || errno != 0) return def;
    return static_cast<int64_t>(v);
  };

  std::string state_str = find_str("state");
  if (state_str == "checking")     g_update_state.state = UpdateState::kChecking;
  else if (state_str == "available")   g_update_state.state = UpdateState::kAvailable;
  else if (state_str == "downloading") g_update_state.state = UpdateState::kDownloading;
  else if (state_str == "ready")       g_update_state.state = UpdateState::kReady;
  else if (state_str == "applying")    g_update_state.state = UpdateState::kApplying;
  else if (state_str == "pending_apply") g_update_state.state = UpdateState::kPendingApply;
  else if (state_str == "error")       g_update_state.state = UpdateState::kError;
  else                                 g_update_state.state = UpdateState::kIdle;

  g_update_state.current_version = find_str("current_version");
  g_update_state.latest_version = find_str("latest_version");
  g_update_state.download_path = find_str("download_path");
  g_update_state.sha256 = find_str("sha256");
  g_update_state.error_message = find_str("error_message");
  g_update_state.proxy = find_str("proxy");
  g_update_state.proxy_type = find_str("proxy_type");
  // proxy_chrome_download is a boolean
  {
    std::string pattern = "\"proxy_chrome_download\":";
    auto s = content.find(pattern);
    if (s != std::string::npos) {
      s += pattern.length();
      g_update_state.proxy_chrome_download = content.substr(s, 4) == "true";
    }
  }
  g_update_state.download_source = (int)find_num("download_source");
  g_update_state.self_latest_version = find_str("self_latest_version");
  g_update_state.self_download_url = find_str("self_download_url");
  g_update_state.self_download_path = find_str("self_download_path");
  g_update_state.self_download_size = find_num("self_download_size");
  g_update_state.self_self_download_progress = (int)find_num("self_download_progress");
  // Self-update ready is a boolean
  {
    std::string pattern = "\"self_update_ready\":";
    auto s = content.find(pattern);
    if (s != std::string::npos) {
      s += pattern.length();
      g_update_state.self_update_ready = content.substr(s, 4) == "true";
    }
  }
  // self_downloading is stored as a boolean (true/false), parse it accordingly
  // rather than via find_num (which would choke on "true").
  {
    std::string pattern = "\"self_downloading\":";
    auto s = content.find(pattern);
    if (s != std::string::npos) {
      s += pattern.length();
      g_update_state.self_downloading = content.substr(s, 4) == "true";
    }
  }
  g_update_state.self_release_notes = find_str("self_release_notes");
  // self_has_update is a boolean
  {
    std::string pattern = "\"self_has_update\":";
    auto s = content.find(pattern);
    if (s != std::string::npos) {
      s += pattern.length();
      g_update_state.self_has_update = content.substr(s, 4) == "true";
    }
  }
  g_update_state.channel = ParseChannel(Utf8ToWstr(find_str("channel")));
  g_update_state.download_progress = (int)find_num("download_progress");
  g_update_state.download_size = find_num("download_size");
  g_update_state.downloaded_bytes = find_num("downloaded_bytes");
  g_update_state.download_speed = find_i64("download_speed", 0);
  g_update_state.download_eta = find_i64("download_eta", 0);
  g_update_state.last_check_time = find_num("last_check_time");
  g_update_state.auto_check = find_num("auto_check") != 0;
  g_update_state.auto_download = find_num("auto_download") != 0;
}

void SaveUpdateState() {
  std::lock_guard<std::mutex> lock(g_update_mutex);

  // Ensure Chrome's data directory (<DLL dir>\..\Data) exists before writing.
  {
    std::wstring data_dir = CanonicalizePath(GetSelfDllDir() + L"\\..\\Data");
    CreateDirectoryW(data_dir.c_str(), nullptr);  // no-op if it already exists
  }

  const char* state_str = "idle";
  switch (g_update_state.state) {
    case UpdateState::kChecking:      state_str = "checking"; break;
    case UpdateState::kAvailable:     state_str = "available"; break;
    case UpdateState::kDownloading:   state_str = "downloading"; break;
    case UpdateState::kReady:         state_str = "ready"; break;
    case UpdateState::kApplying:      state_str = "applying"; break;
    case UpdateState::kPendingApply:  state_str = "pending_apply"; break;
    case UpdateState::kError:         state_str = "error"; break;
    default:                           state_str = "idle"; break;
  }

  std::ostringstream ss;
  ss << "{";
  ss << "\"state\":\"" << state_str << "\",";
  ss << "\"current_version\":\"" << JsonEscape(g_update_state.current_version) << "\",";
  ss << "\"latest_version\":\"" << JsonEscape(g_update_state.latest_version) << "\",";
  ss << "\"channel\":\"" << ChannelToStringA(g_update_state.channel) << "\",";
  ss << "\"download_progress\":" << g_update_state.download_progress << ",";
  ss << "\"download_size\":" << g_update_state.download_size << ",";
  ss << "\"downloaded_bytes\":" << g_update_state.downloaded_bytes << ",";
  ss << "\"download_speed\":" << g_update_state.download_speed << ",";
  ss << "\"download_eta\":" << g_update_state.download_eta << ",";
  ss << "\"download_path\":\"" << JsonEscape(g_update_state.download_path) << "\",";
  ss << "\"sha256\":\"" << JsonEscape(g_update_state.sha256) << "\",";
  ss << "\"error_message\":\"" << JsonEscape(g_update_state.error_message) << "\",";
  ss << "\"last_check_time\":" << g_update_state.last_check_time << ",";
  ss << "\"auto_check\":" << (g_update_state.auto_check ? 1 : 0) << ",";
  ss << "\"auto_download\":" << (g_update_state.auto_download ? 1 : 0) << ",";
  ss << "\"proxy\":\"" << JsonEscape(g_update_state.proxy) << "\",";
  ss << "\"proxy_type\":\"" << JsonEscape(g_update_state.proxy_type) << "\",";
  ss << "\"proxy_chrome_download\":" << (g_update_state.proxy_chrome_download ? "true" : "false") << ",";
  ss << "\"download_source\":" << g_update_state.download_source << ",";
  ss << "\"self_latest_version\":\"" << JsonEscape(g_update_state.self_latest_version) << "\",";
  ss << "\"self_download_url\":\"" << JsonEscape(g_update_state.self_download_url) << "\",";
  ss << "\"self_download_path\":\"" << JsonEscape(g_update_state.self_download_path) << "\",";
  ss << "\"self_download_size\":" << g_update_state.self_download_size << ",";
  ss << "\"self_download_progress\":" << g_update_state.self_self_download_progress << ",";
  ss << "\"self_update_ready\":" << (g_update_state.self_update_ready ? "true" : "false") << ",";
  ss << "\"self_downloading\":" << (g_update_state.self_downloading ? "true" : "false") << ",";
  ss << "\"self_release_notes\":\"" << JsonEscape(g_update_state.self_release_notes) << "\",";
  ss << "\"self_has_update\":" << (g_update_state.self_has_update ? "true" : "false");
  ss << "}";

  std::wstring path = GetUpdateStatePath();
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (file.is_open()) {
    file << ss.str();
  }
}

UpdateStateData GetUpdateStateSnapshot() {
  std::lock_guard<std::mutex> lock(g_update_mutex);
  return g_update_state;
}

void SetUpdateState(UpdateState state) {
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.state = state;
    if (state == UpdateState::kIdle || state == UpdateState::kChecking) {
      g_update_state.error_message.clear();
    }
  }
  SaveUpdateState();
}

void SetUpdateError(const std::string& msg) {
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.state = UpdateState::kError;
    g_update_state.error_message = msg;
  }
  SaveUpdateState();
}

// --- Omaha protocol: check for updates ---

UpdateInfo CheckForUpdates(UpdateChannel channel, UpdateArch arch,
                           const std::string& proxy,
                           const std::string& proxy_type) {
  UpdateInfo info;

  AddDebugLog("Update check started — channel: " + std::string(ChannelToStringA(channel)) +
              ", arch: " + std::string(ArchToString(arch)) +
              (proxy.empty() ? "" : ", proxy: " + proxy));

  // Go reference code sends version="" (empty) — the server returns the latest
  // regardless of what's installed. This is the correct approach.
  std::string request_body = BuildOmahaRequest(channel, arch, "");

  // Determine proxy
  std::wstring proxy_w;

  // Create WinHTTP session
  HINTERNET hSession = WinHttpOpen(L"Google Update/1.3.36.152;winhttp",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    return info;
  }

  // Set proxy: custom > system > none.
  // NOTE: GH_PROXY is intentionally NOT applied here — it only accelerates
  // GitHub *release* downloads (handled in DownloadSelfUpdate). The Omaha check
  // talks to tools.google.com, which a GitHub proxy cannot route.
  if (!proxy.empty() && proxy_type != "GH_PROXY") {
    std::wstring proxy_str(proxy.begin(), proxy.end());
    if (proxy_type == "SOCKS5" && proxy_str.find(L"socks5=") == std::wstring::npos) {
      proxy_str = L"socks5://" + proxy_str;
    } else if (proxy_type == "HTTP" && proxy_str.find(L"http") == std::wstring::npos) {
      proxy_str = L"http://" + proxy_str;
    }
    proxy_str = L"google.com=" + proxy_str;  // hack: set for specific host
    WINHTTP_PROXY_INFO pi;
    pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    pi.lpszProxy = const_cast<LPWSTR>(proxy_str.c_str());
    pi.lpszProxyBypass = nullptr;
    WinHttpSetOption(hSession, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
  } else {
    // Try system proxy
    std::wstring sys_proxy;
    if (GetSystemProxy(sys_proxy) && !sys_proxy.empty()) {
      std::wstring proxy_str = L"http://" + sys_proxy;
      WINHTTP_PROXY_INFO pi;
      pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
      pi.lpszProxy = const_cast<LPWSTR>(proxy_str.c_str());
      pi.lpszProxyBypass = nullptr;
      WinHttpSetOption(hSession, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
    }
  }

  // Set timeout: 15s connect, 15s receive
  WinHttpSetTimeouts(hSession, 15000, 15000, 15000, 15000);

  // Connect to tools.google.com
  HINTERNET hConnect = WinHttpConnect(hSession, L"tools.google.com",
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    return info;
  }

  // Create POST request
  HINTERNET hRequest = WinHttpOpenRequest(
      hConnect, L"POST", L"/service/update2",
      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    AddDebugLog("WinHttpOpenRequest failed");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return info;
  }

  // Set headers
  WinHttpAddRequestHeaders(hRequest,
      L"Content-Type: application/x-www-form-urlencoded",
      (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

  // Send request
  BOOL result = WinHttpSendRequest(hRequest,
      WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      const_cast<char*>(request_body.c_str()),
      (DWORD)request_body.length(),
      (DWORD)request_body.length(), 0);

  if (!result) {
    AddDebugLog("WinHttpSendRequest failed, error: " + std::to_string(GetLastError()));
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return info;
  }

  // Receive response
  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return info;
  }

  // Check HTTP status code
  DWORD status_code = 0;
  DWORD status_code_size = sizeof(status_code);
  if (WinHttpQueryHeaders(hRequest,
      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX,
      &status_code, &status_code_size, WINHTTP_NO_HEADER_INDEX)) {
    if (status_code != 200) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      return info;
    }
  }

  // Read response data
  std::string response;
  DWORD bytes_available = 0;
  while (WinHttpQueryDataAvailable(hRequest, &bytes_available) && bytes_available > 0) {
    std::vector<char> buffer(bytes_available);
    DWORD bytes_read = 0;
    if (WinHttpReadData(hRequest, buffer.data(), bytes_available, &bytes_read) && bytes_read > 0) {
      response.append(buffer.data(), bytes_read);
    } else {
      break;
    }
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  if (response.empty()) {
    AddDebugLog("Omaha returned empty response");
    return info;
  }

  // Verify it's an XML response (Omaha returns XML)
  if (response.find("<?xml") == std::string::npos &&
      response.find("<response") == std::string::npos) {
    // Not an Omaha response — server returned an error page
    AddDebugLog("Omaha response is not XML (error page?), first 200 chars: " +
                response.substr(0, 200));
    return info;
  }


  // The Omaha response contains ALL Google products (380KB+ XML).
  // We MUST locate our specific <app> block by appid before inspecting its status;
  // otherwise status="noupdate" from another product (Google Earth, etc.) would
  // fool us into thinking Chrome has no update.

  const char* appid = GetAppId(channel);

  // Find the <app appid="..."> tag directly (not just appid= which might
  // appear as a string literal inside data elements of other apps).
  std::string app_search = std::string("<app appid=\"") + appid + "\"";
  auto tag_start = response.find(app_search);
  if (tag_start == std::string::npos) {
    // Fallback: search for just the appid in case of attribute reordering
    app_search = std::string("appid=\"") + appid + "\"";
    tag_start = response.find(app_search);
    if (tag_start == std::string::npos) {
      AddDebugLog("App block not found in Omaha response");
      return info;
    }
    // Walk back to find the <app tag start
    auto backtrack = response.rfind("<app", tag_start);
    if (backtrack != std::string::npos) tag_start = backtrack;
  }

  // Find the end of OUR app block.
  // Strategy: find our </app> by looking for the NEXT <app> after us,
  // then step back to the preceding </app>. This handles the case where
  // other apps' data contains "</app>" as text (unlikely but safe).
  auto next_app = response.find("<app", tag_start + 4);
  auto app_end = response.find("</app>", tag_start);
  // If a next app starts before our closing tag, use the next app's start as boundary
  if (next_app != std::string::npos && next_app < app_end) {
    // Script unlikely; trust the first </app> after our appid
    app_end = response.rfind("</app>", next_app);
    if (app_end == std::string::npos) app_end = next_app;
  }
  // Make sure app_end is reasonable
  if (app_end == std::string::npos || app_end < tag_start) {
    app_end = response.length();
  }
  app_end += 6;  // include "</app>"

  std::string app_block = response.substr(tag_start, app_end - tag_start);

  // (Raw XML is intentionally NOT logged — only parsed results are recorded below.)

  // We do NOT check for status="noupdate" here.
  // The Go reference code proves that the correct approach is to simply
  // parse the version from the manifest and compare it with the installed
  // version ourselves. The server always returns the latest version for
  // the given channel+arch regardless of what's installed.


  // Extract version directly from the <manifest> opening tag.
  // ExtractTag() returns content between tags, but version is an ATTRIBUTE
  // on the opening tag, so we parse the tag itself instead.
  {
    auto m_start = app_block.find("<manifest ");
    if (m_start != std::string::npos) {
      auto m_end = app_block.find('>', m_start);
      if (m_end != std::string::npos) {
        std::string open_tag = app_block.substr(m_start, m_end - m_start + 1);
        info.version = ExtractAttr(open_tag, "version");
      }
    }
  }

  // Extract package info — <package> is self-closing (<package .../>),
  // so ExtractTag won't work. Parse attributes directly from the opening tag.
  std::string name;
  std::string size_str;
  {
    auto p_start = app_block.find("<package ");
    if (p_start != std::string::npos) {
      auto p_end = app_block.find("/>", p_start);
      if (p_end == std::string::npos) p_end = app_block.find('>', p_start);
      if (p_end != std::string::npos) {
        std::string pkg_tag = app_block.substr(p_start, p_end - p_start + 2);
        name = ExtractAttr(pkg_tag, "name");
        size_str = ExtractAttr(pkg_tag, "size");
        info.sha256 = ExtractAttr(pkg_tag, "hash_sha256");
        if (info.sha256.empty()) info.sha256 = ExtractAttr(pkg_tag, "hashSha256");
      }
    }
  }

  // Parse size
  if (!size_str.empty()) {
    char* end = nullptr;
    long long val = strtoll(size_str.c_str(), &end, 10);
    if (end != size_str.c_str() && *end == '\0') {
      info.size = val;
    }
  }

  // Extract download URLs — search within our app block
  std::string urls_block = ExtractTag(app_block, "urls");
  // Find all <url codebase="..."/> within urls_block
  {
    std::string search = "<url";
    size_t pos = 0;
    while ((pos = urls_block.find(search, pos)) != std::string::npos) {
      auto tag_end = urls_block.find("/>", pos);
      if (tag_end == std::string::npos) break;
      std::string tag = urls_block.substr(pos, tag_end - pos + 2);
      std::string codebase = ExtractAttr(tag, "codebase");
      if (!codebase.empty()) {
        info.urls.push_back(codebase + name);
      }
      pos = tag_end + 2;
    }
  }

  info.timestamp = static_cast<int64_t>(time(nullptr)) * 1000;
  info.has_update = !info.version.empty() && !info.urls.empty();

  if (info.has_update) {
    AddDebugLog("Update found: " + info.version + " (" + std::to_string(info.urls.size()) +
                " URLs, " + std::to_string(info.size) + " bytes)");
  } else {
    AddDebugLog("No update found (version_empty=" +
                std::string(info.version.empty() ? "true" : "false") +
                ", urls_empty=" + std::string(info.urls.empty() ? "true" : "false") + ")");
  }

  return info;
}

// --- Self-update helpers ---

// Compute the actual (host, path, https) to request for a GitHub-origin URL,
// honoring the proxy config. Both the version CHECK and the download must be
// able to reach GitHub in blocked regions, so GH_PROXY is applied here too:
//   - GH_PROXY: rewrite the URL through the mirror (proxy baked into the URL,
//     so no WinHTTP proxy); the connection goes to the mirror host.
//   - HTTP/SOCKS5: set the WinHTTP named proxy and use the original host/path.
// Forward declaration — BuildGhProxyUrl is defined later (near DownloadSelfUpdate).
static std::string BuildGhProxyUrl(const std::string& proxy,
                                   const std::string& url);
static bool CheckFetchTarget(HINTERNET hSession,
                             const std::string& url,
                             const std::string& proxy,
                             const std::string& proxy_type,
                             std::string& out_host,
                             std::string& out_path) {
  std::string effective = url;
  if (proxy_type == "GH_PROXY" && !proxy.empty()) {
    effective = BuildGhProxyUrl(proxy, url);
    AddDebugLog("GH_PROXY check URL: " + effective);
  } else if (!proxy.empty()) {
    std::wstring proxy_w(proxy.begin(), proxy.end());
    if (proxy_type == "HTTP" && proxy_w.find(L"http") == std::wstring::npos) {
      proxy_w = L"http://" + proxy_w;
    }
    WINHTTP_PROXY_INFO pi;
    pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    pi.lpszProxy = const_cast<LPWSTR>(proxy_w.c_str());
    pi.lpszProxyBypass = nullptr;
    WinHttpSetOption(hSession, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
  }
  std::string u = effective;
  bool https = true;
  if (u.find("https://") == 0) u = u.substr(8);
  else if (u.find("http://") == 0) { u = u.substr(7); https = false; }
  auto slash = u.find('/');
  if (slash == std::string::npos) { out_host = u; out_path = "/"; }
  else { out_host = u.substr(0, slash); out_path = u.substr(slash); }
  return https;
}

// Compare two semver strings "MAIN.SUB.FIX[-pre]". Returns >0 if a>b, <0 if a<b,
// 0 if equal. A release build (no suffix) sorts higher than a pre-release with
// the same numeric components (e.g. "1.0.0" > "1.0.0-beta"). Used to decide
// whether a published Release is newer than the locally compiled version.
static int CompareSemver(const std::string& a, const std::string& b) {
  auto parse = [](const std::string& v, int out[3], std::string& pre) {
    pre.clear();
    size_t i = 0;
    std::string nums;
    while (i < v.size() && ((v[i] >= '0' && v[i] <= '9') || v[i] == '.')) {
      nums.push_back(v[i]);
      i++;
    }
    if (i < v.size()) pre = v.substr(i);
    out[0] = out[1] = out[2] = 0;
    int idx = 0;
    std::string cur;
    for (char c : nums) {
      if (c == '.') {
        if (idx < 3) out[idx++] = (int)strtol(cur.c_str(), nullptr, 10);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty() && idx < 3) out[idx++] = (int)strtol(cur.c_str(), nullptr, 10);
  };
  int ax[3], bx[3];
  std::string ap, bp;
  parse(a, ax, ap);
  parse(b, bx, bp);
  for (int i = 0; i < 3; i++) {
    if (ax[i] != bx[i]) return ax[i] < bx[i] ? -1 : 1;
  }
  // Same numeric components: a release (no suffix) is newer than a pre-release.
  if (ap.empty() && !bp.empty()) return 1;
  if (!ap.empty() && bp.empty()) return -1;
  if (ap < bp) return -1;
  if (ap > bp) return 1;
  return 0;
}

// --- Self-update: release metadata via a pre-baked static JSON ---
//
// Instead of hitting the (60/h rate-limited) REST API or the web 302 redirect,
// we fetch a static JSON file generated by a GitHub Action
// (libsgh/ghapi-json-generator) that mirrors the output of
//   GET /repos/<owner>/<repo>/releases?per_page=10
// served from raw.githubusercontent / github.com raw. This is CDN-cached and
// quota-free. The JSON is an ARRAY of releases (newest first), each with
// tag_name / prerelease / draft / assets[].browser_download_url / size.
//
// We walk the array, skip draft & pre-release entries, and pick the highest
// stable version via CompareSemver (the array is usually already sorted, but
// CompareSemver guards against ordering surprises). The download URL and size
// come straight from the chosen release's assets, so no second request is
// needed. The fetch still honors the proxy config through CheckFetchTarget /
// BuildGhProxyUrl exactly like the old paths did.

static const char* kReleaseIndexBase =
    "https://github.com/libsgh/ghapi-json-generator/raw/refs/heads/output/v2/"
    "repos/libsgh/chrome_green/releases%3Fper_page=10/data.json";

// Minimal string-search JSON helpers (no external parser dependency).

// Return the substring between the first "key": and the next unescaped '"'.
// Tolerates the whitespace GitHub's API puts after the colon
// (e.g. "tag_name": "2.0.0"). Returns "" if not found.
static std::string JsonStringField(const std::string& s,
                                   const std::string& key) {
  std::string pat = "\"" + key + "\":";
  auto p = s.find(pat);
  if (p == std::string::npos) return "";
  p += pat.size();
  // Skip any whitespace between ':' and the opening quote.
  while (p < s.size() &&
         (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) {
    p++;
  }
  if (p >= s.size() || s[p] != '"') return "";
  p += 1;  // skip opening quote
  std::string out;
  while (p < s.size()) {
    char c = s[p];
    if (c == '\\' && p + 1 < s.size()) {
      char n = s[p + 1];
      if (n == 'n') out.push_back('\n');
      else if (n == 't') out.push_back('\t');
      else if (n == 'r') out.push_back('\r');
      else if (n == '\\') out.push_back('\\');
      else if (n == '"') out.push_back('"');
      else { out.push_back(c); out.push_back(n); }
      p += 2;
    } else if (c == '"') {
      break;
    } else {
      out.push_back(c);
      p += 1;
    }
  }
  return out;
}

// Read a JSON integer field ("key":<number>) as a 64-bit signed value.
// Returns false if not found / not parseable.
static bool JsonIntField(const std::string& s, const std::string& key,
                         long long& out) {
  std::string pat = "\"" + key + "\":";
  auto p = s.find(pat);
  if (p == std::string::npos) return false;
  p += pat.size();
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
  char* end = nullptr;
  errno = 0;
  long long v = strtoll(s.c_str() + p, &end, 10);
  if (end == s.c_str() + p || end == nullptr) return false;
  out = v;
  return true;
}

// Parse the release-index array and fill info with the highest stable release.
// On success sets version, urls (matching ChromeGreen_v*.zip asset), size and
// has_update (true only if remote > local RELEASE_VER_STR). Returns false on
// any structural failure (empty body, no stable release found).
static bool ParseReleaseIndex(const std::string& body, UpdateInfo& info) {
  if (body.empty()) return false;

  // The body is a JSON array. Walk top-level release objects by locating each
  // '{' that opens a release entry. We approximate by scanning for
  // "tag_name":" occurrences, then taking the substring up to the matching
  // next top-level '}' is hard without a real parser, so instead we use a
  // simpler, robust approach: split on the asset delimiter.
  //
  // Each release contains the fields we need in order:
  //   "tag_name":"<tag>",
  //   "prerelease":<bool>,
  //   "draft":<bool>,
  //   "assets":[ { "browser_download_url":"<url>", "size":<n>, ... }, ... ]
  // We iterate by searching for each "tag_name":" and then scanning forward to
  // the NEXT "tag_name":" (or end) as this entry's slice.

  std::string tag_key = "\"tag_name\":";
  std::vector<std::pair<size_t, size_t>> entries;  // [start, end) slices
  size_t pos = 0;
  while ((pos = body.find(tag_key, pos)) != std::string::npos) {
    // Skip the optional space after ':' and confirm the opening quote, so we
    // only treat real "tag_name" fields as release boundaries (GitHub JSON
    // emits "tag_name": "...").
    size_t q = pos + tag_key.size();
    while (q < body.size() &&
           (body[q] == ' ' || body[q] == '\t' || body[q] == '\n' || body[q] == '\r')) {
      q++;
    }
    if (q >= body.size() || body[q] != '"') {
      pos = (q >= body.size()) ? body.size() : q;
      continue;
    }
    size_t start = pos;
    // find the next tag_name after this one
    size_t next = body.find(tag_key, q);
    size_t end = (next == std::string::npos) ? body.size() : next;
    entries.emplace_back(start, end);
    pos = (next == std::string::npos) ? body.size() : next;
  }

  if (entries.empty()) {
    AddDebugLog("Self-update parse: no tag_name entries found");
    return false;
  }

  bool found_any = false;
  for (const auto& e : entries) {
    std::string slice = body.substr(e.first, e.second - e.first);

    std::string tag = JsonStringField(slice, "tag_name");
    if (tag.empty()) continue;
    // Normalize: strip a leading "v".
    if (tag[0] == 'v') tag = tag.substr(1);

    // Skip draft / pre-release.
    long long pre = 0, draft = 0;
    JsonIntField(slice, "prerelease", pre);
    JsonIntField(slice, "draft", draft);
    if (pre != 0 || draft != 0) {
      AddDebugLog("Self-update parse: skip draft/prerelease tag " + tag);
      continue;
    }

    // Choose the highest stable version seen so far.
    if (!found_any || CompareSemver(tag, info.version) > 0) {
      // Find the matching asset URL + size within this slice.
      std::string want = "ChromeGreen_v";
      std::string url;
      long long size = 0;
      std::string search = "\"browser_download_url\":";
      size_t ap = 0;
      while ((ap = slice.find(search, ap)) != std::string::npos) {
        std::string u =
            JsonStringField(slice.substr(ap), "browser_download_url");
        if (!u.empty() && u.find(want) != std::string::npos &&
            u.size() >= 4 && u.compare(u.size() - 4, 4, ".zip") == 0) {
          url = u;
          // size is reported just before this asset block; scan backwards.
          std::string ss = "\"size\":";
          auto sp = slice.rfind(ss, ap);
          if (sp != std::string::npos) {
            long long sz = 0;
            if (JsonIntField(slice.substr(sp), "size", sz)) size = sz;
          }
          break;
        }
        ap += search.size();
      }
      if (url.empty()) {
        AddDebugLog("Self-update parse: tag " + tag + " has no ChromeGreen_v*.zip asset");
        continue;
      }

      info.version = tag;
      info.urls.clear();
      info.urls.push_back(url);
      info.size = size;
      found_any = true;
      AddDebugLog("Self-update parse: candidate stable tag " + tag +
                  " (size=" + std::to_string(size) + ")");
    }
  }

  if (!found_any) {
    AddDebugLog("Self-update parse: no stable release with a matching asset");
    return false;
  }

  // Backend-driven comparison against the locally compiled version.
  info.has_update = CompareSemver(info.version, RELEASE_VER_STR) > 0;
  if (info.has_update) {
    AddDebugLog("Self-update: remote " + info.version + " > local " +
                std::string(RELEASE_VER_STR) + " -> update available");
  } else {
    AddDebugLog("Self-update: local " + std::string(RELEASE_VER_STR) +
                " is current (remote " + info.version + ") -> no update");
    info.urls.clear();
    info.size = 0;
  }
  return true;
}

// Fetch the static release-index JSON and parse it.
static UpdateInfo FetchSelfUpdateIndex(const std::string& proxy,
                                       const std::string& proxy_type) {
  UpdateInfo info;

  HINTERNET hSession = WinHttpOpen(L"ChromeGreen SelfUpdate",
      WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return info;

  std::string host, path;
  bool https = CheckFetchTarget(hSession, kReleaseIndexBase,
                                proxy, proxy_type, host, path);
  WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

  HINTERNET hConnect = WinHttpConnect(hSession, Utf8ToWstr(host).c_str(),
                                      https ? INTERNET_DEFAULT_HTTPS_PORT
                                            : INTERNET_DEFAULT_HTTP_PORT,
                                      0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    return info;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
      Utf8ToWstr(path).c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      https ? WINHTTP_FLAG_SECURE : 0);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return info;
  }

  WinHttpAddRequestHeaders(hRequest,
      L"User-Agent: ChromeGreen\r\nAccept: application/json",
      (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

  if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return info;
  }
  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return info;
  }

  std::string body;
  DWORD bytes_available = 0;
  while (WinHttpQueryDataAvailable(hRequest, &bytes_available) &&
         bytes_available > 0) {
    std::vector<char> buffer(bytes_available);
    DWORD bytes_read = 0;
    if (WinHttpReadData(hRequest, buffer.data(), bytes_available, &bytes_read) &&
        bytes_read > 0) {
      body.append(buffer.data(), bytes_read);
    } else {
      break;
    }
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  if (!ParseReleaseIndex(body, info)) {
    AddDebugLog("Self-update check failed: could not parse release index");
  }
  return info;
}

// --- Self-update: detect a newer version.dll via published GitHub Releases ---
//
// The check is based ONLY on published Releases (never on the main branch's
// src/version.h, which can be bumped before a Release actually ships). Release
// metadata is fetched from a pre-baked static JSON (libsgh/ghapi-json-generator)
// that mirrors GET /repos/<owner>/<repo>/releases?per_page=10 — CDN-cached and
// free of the 60/h REST quota. The array is walked for the highest stable
// (non-draft, non-prerelease) version, whose ChromeGreen_v*.zip asset gives the
// download URL + size in a single request.
//
// The local-vs-remote comparison (the "is there a newer version?" decision)
// lives entirely on the backend: we compare the resolved release tag against
// the locally compiled RELEASE_VER_STR and only flag has_update when the
// published version is strictly greater.
UpdateInfo CheckSelfUpdate(const std::string& proxy,
                          const std::string& proxy_type) {
  AddDebugLog("Self-update check started (release index)" +
              (proxy.empty() ? "" : ", proxy: " + proxy));

  UpdateInfo info = FetchSelfUpdateIndex(proxy, proxy_type);

  if (info.version.empty()) {
    AddDebugLog("Self-update check failed: no published release version found");
    return info;  // has_update stays false
  }
  return info;
}


// Build a GitHub-proxy accelerated URL: <proxy_base>/<original_url>.
// Tolerates a trailing slash on proxy_base so both
//   "https://gh.noki.eu.org"        and  "https://gh.noki.eu.org/"
// produce "https://gh.noki.eu.org/https://github.com/...".
static std::string BuildGhProxyUrl(const std::string& proxy,
                                   const std::string& url) {
  if (proxy.empty() || url.empty()) return url;
  std::string base = proxy;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + "/" + url;
}

// Return the architecture folder name that matches the current process.
static std::wstring GetCurrentArchitectureFolder() {
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      return L"x64";
    case PROCESSOR_ARCHITECTURE_INTEL:
      return L"x86";
    case PROCESSOR_ARCHITECTURE_ARM64:
      return L"arm64";
    default:
      return L"";
  }
}

// Extract a .zip archive using the built-in tar.exe (Windows 10 17063+).
// Returns true if tar reports success and at least one file was extracted.
static bool ExtractSelfUpdateZip(const std::wstring& zip_path,
                                 const std::wstring& out_dir) {
  CreateDirectoryW(out_dir.c_str(), nullptr);

  // Build command line: tar.exe -xf "zip" -C "outdir"
  std::wstring cmd = L"tar.exe -xf \"" + zip_path + L"\" -C \"" + out_dir + L"\"";

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};

  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    AddDebugLog("ExtractSelfUpdateZip: CreateProcessW failed, error=" +
                std::to_string(GetLastError()));
    return false;
  }

  // Wait up to 60 seconds for extraction
  DWORD wait = WaitForSingleObject(pi.hProcess, 60000);
  DWORD exit_code = ERROR_TIMEOUT;
  if (wait == WAIT_OBJECT_0) {
    GetExitCodeProcess(pi.hProcess, &exit_code);
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  if (exit_code != 0) {
    AddDebugLog("ExtractSelfUpdateZip: tar exited with " +
                std::to_string(exit_code));
    return false;
  }

  // The release zip nests version.dll under an architecture folder
  // (x64/x86/arm64). Locate the one matching the current process and move it
  // to the root of out_dir so the rest of the update pipeline can use the
  // fixed path "updates\\self_update\\version.dll".
  std::wstring arch = GetCurrentArchitectureFolder();
  std::wstring src;
  if (!arch.empty()) {
    std::wstring nested = out_dir + L"\\" + arch + L"\\version.dll";
    DWORD attr = GetFileAttributesW(nested.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      src = nested;
    }
  }
  if (src.empty()) {
    std::wstring root = out_dir + L"\\version.dll";
    DWORD attr = GetFileAttributesW(root.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      src = root;
    }
  }
  if (src.empty()) {
    AddDebugLog("ExtractSelfUpdateZip: version.dll not found after extraction");
    return false;
  }

  std::wstring dst = out_dir + L"\\version.dll";
  if (_wcsicmp(src.c_str(), dst.c_str()) != 0) {
    DeleteFileW(dst.c_str());
    if (!MoveFileW(src.c_str(), dst.c_str())) {
      AddDebugLog("ExtractSelfUpdateZip: MoveFileW failed, error=" +
                  std::to_string(GetLastError()));
      return false;
    }
  }
  return true;
}

bool DownloadSelfUpdate(const std::string& url,
                        const std::wstring& save_path,
                        const std::wstring& extract_dir,
                        const std::string& proxy,
                        const std::string& proxy_type) {
  // GH_PROXY: accelerate GitHub *release* downloads by rewriting the URL to go
  // through a public GitHub proxy (e.g. https://gh.noki.eu.org). The proxy host
  // is baked into the URL itself, so we do NOT set a WinHTTP proxy. This type
  // ONLY proxies GitHub release URLs — Chrome installer downloads never use it
  // (see downloader.cc DownloadThread).
  std::string effective_url = url;
  std::string effective_proxy = proxy;
  if (proxy_type == "GH_PROXY") {
    effective_url = BuildGhProxyUrl(proxy, url);
    effective_proxy.clear();
    AddDebugLog("GH_PROXY self-update download URL: " + effective_url);
  }

  HINTERNET hSession = WinHttpOpen(
      L"ChromeGreen SelfUpdate",
      WINHTTP_ACCESS_TYPE_NO_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return false;

  // Set proxy if configured (skipped for GH_PROXY — the proxy is in the URL)
  if (!effective_proxy.empty()) {
    std::wstring proxy_w(effective_proxy.begin(), effective_proxy.end());
    if (proxy_type == "HTTP" && proxy_w.find(L"http") == std::wstring::npos) {
      proxy_w = L"http://" + proxy_w;
    }
    WINHTTP_PROXY_INFO pi;
    pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    pi.lpszProxy = const_cast<LPWSTR>(proxy_w.c_str());
    pi.lpszProxyBypass = nullptr;
    WinHttpSetOption(hSession, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
  }

  // Parse URL to get host, port and path.
  // URL format (after a possible GH_PROXY rewrite):
  //   https://gh.noki.eu.org/https://github.com/...
  // The port MUST be taken from the URL when present — GitHub proxies and local
  // test servers routinely listen on non-default ports, and a hardcoded
  // INTERNET_DEFAULT_*_PORT would silently misconnect (e.g. 127.0.0.1:18888 ->
  // :80) and fail the download.
  std::string host = "github.com";
  std::string path = effective_url;
  bool https = true;

  if (effective_url.find("https://") == 0) {
    path = effective_url.substr(8);
  } else if (effective_url.find("http://") == 0) {
    path = effective_url.substr(7);
    https = false;
  }
  auto slash = path.find('/');
  if (slash != std::string::npos) {
    host = path.substr(0, slash);
    path = path.substr(slash);
  }

  // Split an explicit ":port" suffix off the host (defaults if absent).
  int port = https ? 443 : 80;
  auto colon = host.rfind(':');
  if (colon != std::string::npos) {
    errno = 0;
    char* endp = nullptr;
    long v = strtol(host.c_str() + colon + 1, &endp, 10);
    if (endp != host.c_str() + colon + 1 && *endp == '\0' && v > 0 && v <= 65535) {
      port = (int)v;
      host = host.substr(0, colon);
    }
  }

  std::wstring host_w(host.begin(), host.end());
  std::wstring path_w(path.begin(), path.end());

  HINTERNET hConnect = WinHttpConnect(hSession, host_w.c_str(),
      (INTERNET_PORT)port, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    return false;
  }

  DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path_w.c_str(),
      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  // Query total size for progress reporting
  DWORD content_len = 0;
  DWORD cl_size = sizeof(content_len);
  DWORD total_size = 0;
  if (WinHttpQueryHeaders(hRequest,
      WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &content_len, &cl_size,
      WINHTTP_NO_HEADER_INDEX)) {
    total_size = content_len;
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.self_download_size = total_size;
    }
    SaveUpdateState();
  }

  HANDLE hFile = CreateFileW(save_path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  DWORD bytes_available = 0;
  DWORD downloaded = 0;
  DWORD last_reported_pct = 0;
  while (WinHttpQueryDataAvailable(hRequest, &bytes_available) &&
         bytes_available > 0) {
    std::vector<char> buffer(bytes_available);
    DWORD bytes_read = 0;
    if (WinHttpReadData(hRequest, buffer.data(), bytes_available, &bytes_read) &&
        bytes_read > 0) {
      DWORD written = 0;
      WriteFile(hFile, buffer.data(), bytes_read, &written, nullptr);
      downloaded += bytes_read;
      if (total_size > 0) {
        int pct = (int)((uint64_t)downloaded * 100ULL / (uint64_t)total_size);
        if (pct != last_reported_pct) {
          last_reported_pct = pct;
          {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            g_update_state.self_self_download_progress = pct;
          }
          SaveUpdateState();
        }
      }
    }
  }

  CloseHandle(hFile);
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  // Extract the downloaded zip to get version.dll
  if (!ExtractSelfUpdateZip(save_path, extract_dir)) {
    return false;
  }

  // Update state: extracted version.dll is ready
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.self_self_download_progress = 100;
  }
  SaveUpdateState();
  return true;
}
