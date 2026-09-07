#include "httpserver.h"

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "hosts_manager.h"
#include "keycapture.h"
#include "downloader.h"
#include "appid.h"
#include "tools.h"
#include "com_initializer.h"
#include "diaglog.h"
#include "update.h"
#include "updater.h"
#include "utils.h"
#include "version.h"
#include "web_content.h"

#pragma comment(lib, "ws2_32.lib")

// Per-install config-server socket + port. Reserved early (before PakPatch)
// so the UI links and the live server always agree, and held open so no
// other local service can steal the port out from under us.
static SOCKET g_listen_socket = INVALID_SOCKET;
static int g_config_port = 0;

namespace {

std::atomic<bool> server_running_{false};
std::thread server_thread_;

// Recursively remove a directory and its contents (small local helper).
void RemoveDirectoryRecursiveLocal(const std::wstring& dir) {
  std::wstring search = dir + L"\\*";
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    RemoveDirectoryW(dir.c_str());
    return;
  }
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring path = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      RemoveDirectoryRecursiveLocal(path);
    } else {
      SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(path.c_str());
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  RemoveDirectoryW(dir.c_str());
}

// --- HTTP request structure ---

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
  std::string content_type;
};

// Parse an HTTP request from raw bytes.
// Returns true if a complete request was parsed.
bool ParseRequest(const std::string& raw, HttpRequest& req) {
  auto header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) return false;

  std::string header = raw.substr(0, header_end);
  req.body = raw.substr(header_end + 4);

  // Parse request line
  auto first_line_end = header.find("\r\n");
  std::string request_line = header.substr(0, first_line_end);

  auto sp1 = request_line.find(' ');
  auto sp2 = request_line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return false;

  req.method = request_line.substr(0, sp1);
  req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

  // Parse Content-Type
  std::string lower_header = header;
  std::transform(lower_header.begin(), lower_header.end(),
                  lower_header.begin(), ::tolower);
  auto ct_pos = lower_header.find("content-type:");
  if (ct_pos != std::string::npos) {
    auto line_end = lower_header.find("\r\n", ct_pos);
    req.content_type = header.substr(ct_pos + 14, line_end - ct_pos - 14);
    // Trim whitespace
    while (!req.content_type.empty() && req.content_type[0] == ' ')
      req.content_type.erase(0, 1);
  }

  return true;
}

// --- JSON response helpers ---

std::string GetStatusJson() {
  auto s = GetUpdateStateSnapshot();
  const char* state_str = "idle";
  switch (s.state) {
    case UpdateState::kChecking:      state_str = "checking"; break;
    case UpdateState::kAvailable:     state_str = "available"; break;
    case UpdateState::kDownloading:   state_str = "downloading"; break;
    case UpdateState::kReady:         state_str = "ready"; break;
    case UpdateState::kApplying:      state_str = "applying"; break;
    case UpdateState::kPendingApply:  state_str = "pending_apply"; break;
    case UpdateState::kError:         state_str = "error"; break;
    default: break;
  }

  std::ostringstream ss;
  ss << "{";
  ss << "\"state\":\"" << state_str << "\",";
  ss << "\"current_version\":\"" << s.current_version << "\",";
  ss << "\"latest_version\":\"" << s.latest_version << "\",";
  ss << "\"channel\":\"" << ChannelToStringA(s.channel) << "\",";
  ss << "\"arch\":\"" << ArchToString(s.arch) << "\",";
  ss << "\"download_progress\":" << s.download_progress << ",";
  ss << "\"download_size\":" << s.download_size << ",";
  ss << "\"downloaded_bytes\":" << s.downloaded_bytes << ",";
  ss << "\"download_speed\":" << s.download_speed << ",";
  ss << "\"download_eta\":" << s.download_eta << ",";
  ss << "\"error_message\":\"" << s.error_message << "\",";
  ss << "\"last_check_time\":" << s.last_check_time << ",";
  ss << "\"auto_check\":" << (s.auto_check ? "true" : "false") << ",";
  ss << "\"auto_download\":" << (s.auto_download ? "true" : "false") << ",";
  ss << "\"proxy\":\"" << s.proxy << "\",";
  ss << "\"proxy_type\":\"" << s.proxy_type << "\",";
  ss << "\"proxy_chrome_download\":" << (s.proxy_chrome_download ? "true" : "false") << ",";
  ss << "\"download_source\":" << s.download_source << ",";
  ss << "\"chrome_green_version\":\"" << RELEASE_VER_STR "\",";
  // Self-update fields
  ss << "\"self_latest_version\":\"" << s.self_latest_version << "\",";
  ss << "\"self_download_size\":" << s.self_download_size << ",";
  ss << "\"self_download_progress\":" << s.self_self_download_progress << ",";
  ss << "\"self_update_ready\":" << (s.self_update_ready ? "true" : "false") << ",";
  ss << "\"self_downloading\":" << (s.self_downloading ? "true" : "false") << ",";
  ss << "\"self_release_notes\":\"" << s.self_release_notes << "\",";
  ss << "\"self_has_update\":" << (s.self_has_update ? "true" : "false") << ",";
  ss << "\"sha256\":\"" << s.sha256 << "\",";
  // When the user chooses to keep installer files, a package already sitting in
  // updates/ is expected (it's the retained installer), so don't surface the
  // "local package found 鈥?install offline" prompt in that case.
  bool show_local_pkg = HasLocalPackage() && !config.KeepInstaller();
  ss << "\"has_local_package\":" << (show_local_pkg ? "true" : "false") << ",";
  // Check if a pending chrome.exe swap exists (from a previous failed update)
  std::wstring new_exe = GetAppDir() + L"\\chrome.exe.new";
  ss << "\"has_pending_swap\":" << (PathFileExistsW(new_exe.c_str()) ? "true" : "false");
  ss << "}";
  return ss.str();
}

std::string GetConfigJson() {
  std::ostringstream ss;
  ss << "{";
  ss << "\"channel\":\"" << ChannelToStringA(config.GetUpdateChannel()) << "\",";
  ss << "\"arch\":\"" << ArchToString(config.GetUpdateArch()) << "\",";
  ss << "\"auto_check\":" << (config.IsAutoCheck() ? "true" : "false") << ",";
  ss << "\"check_interval\":" << config.GetCheckInterval() << ",";
  ss << "\"auto_download\":" << (config.IsAutoDownload() ? "true" : "false") << ",";
  ss << "\"proxy\":\"" << JsonEscape(config.GetUpdateProxy()) << "\",";
  ss << "\"proxy_type\":\"" << JsonEscape(config.GetUpdateProxyType()) << "\",";
  ss << "\"proxy_chrome_download\":" << (g_update_state.proxy_chrome_download ? "true" : "false") << ",";
  ss << "\"download_source\":" << g_update_state.download_source << ",";
  ss << "\"keep_installer\":" << (config.KeepInstaller() ? "true" : "false") << ",";
  ss << "\"keep_old_versions\":" << (config.KeepOldVersions() ? "true" : "false") << ",";
  ss << "\"theme\":\"" << JsonEscape(config.GetTheme()) << "\",";
  ss << "\"language\":\"" << JsonEscape(config.GetLanguage()) << "\",";
  // --- general (portable / launch / hotkeys) ---
  ss << "\"data_dir\":\"" << JsonEscape(WStringToUtf8(config.GetUserDataDirRaw())) << "\",";
  ss << "\"cache_dir\":\"" << JsonEscape(WStringToUtf8(config.GetDiskCacheDirRaw())) << "\",";
  ss << "\"command_line\":\"" << JsonEscape(WStringToUtf8(config.GetCommandLine())) << "\",";
  ss << "\"launch_on_startup\":\"" << JsonEscape(WStringToUtf8(config.GetLaunchOnStartup())) << "\",";
  ss << "\"launch_on_exit\":\"" << JsonEscape(WStringToUtf8(config.GetLaunchOnExit())) << "\",";
  ss << "\"translate_key\":\"" << JsonEscape(WStringToUtf8(config.GetTranslateKey())) << "\",";
  ss << "\"boss_key\":\"" << JsonEscape(WStringToUtf8(config.GetBossKey())) << "\",";
  // --- action hotkeys (open new window / batch-open URL group) ---
  ss << "\"open_new_window\":\"" << JsonEscape(WStringToUtf8(config.GetOpenNewWindowHotkey())) << "\",";
  ss << "\"open_url_group\":\"" << JsonEscape(WStringToUtf8(config.GetOpenUrlGroupHotkey())) << "\",";
  {
    std::string ug;
    for (const auto& u : config.GetUrlGroup()) {
      if (!ug.empty()) ug += '\n';
      ug += WStringToUtf8(u);
    }
    ss << "\"url_group\":\"" << JsonEscape(ug) << "\",";
  }
  ss << "\"win32k\":" << (config.IsWin32K() ? "true" : "false") << ",";
  ss << "\"ignore_policies\":" << (config.IsIgnorePolicies() ? "true" : "false") << ",";
  ss << "\"suppress_false_upgrade_notification\":" << (config.IsSuppressFalseUpgradeNotification() ? "true" : "false") << ",";
  ss << "\"show_password\":" << (config.IsShowPassword() ? "true" : "false") << ",";
  ss << "\"debug_log\":" << (config.IsDebugLog() ? "true" : "false") << ",";
  ss << "\"suppress_cmdline_warning\":" << (config.IsSuppressCmdlineWarning() ? "true" : "false") << ",";
  ss << "\"open_config_after_update\":" << (config.IsOpenConfigAfterUpdate() ? "true" : "false") << ",";
  ss << "\"fix_taskbar_menu\":" << (config.IsFixTaskbarMenu() ? "true" : "false") << ",";
  // --- tabs (ported from chrome_plus tabbookmark) ---
  ss << "\"keep_last_tab\":" << (config.IsKeepLastTab() ? "true" : "false") << ",";
  ss << "\"double_click_close\":" << (config.IsDoubleClickClose() ? "true" : "false") << ",";
  ss << "\"right_click_close\":" << (config.IsRightClickClose() ? "true" : "false") << ",";
  ss << "\"wheel_tab\":" << (config.IsWheelTab() ? "true" : "false") << ",";
  ss << "\"wheel_tab_when_press_rbutton\":" << (config.IsWheelTabWhenPressRightButton() ? "true" : "false") << ",";
  ss << "\"hover_tab\":" << (config.IsHoverTab() ? "true" : "false") << ",";
  ss << "\"hover_tab_delay\":" << config.GetHoverTabDelay() << ",";
  ss << "\"open_url_new_tab\":" << config.GetOpenUrlNewTabMode() << ",";
  ss << "\"open_bookmark_new_tab\":" << config.GetBookmarkNewTabMode() << ",";
  ss << "\"new_tab_disable\":" << (config.IsNewTabDisable() ? "true" : "false") << ",";
  ss << "\"new_tab_disable_name\":\"" << JsonEscape(WStringToUtf8(config.GetDisableTabName())) << "\",";
  // --- keymapping (each entry "src=dst", joined by newline) ---
  {
    std::string km;
    for (const auto& [src, dst] : config.GetKeyMappings()) {
      if (!km.empty()) km += '\n';
      km += WStringToUtf8(src) + "=" + WStringToUtf8(dst);
    }
    ss << "\"key_mappings\":\"" << JsonEscape(km) << "\"";
  }
  ss << "}";
  return ss.str();
}

// Build the JSON for the domain-mapping (域名映射) config page: global config,
// the subscription list (each with its own cached rules for the expandable
// panels), and the aggregated effective rules (for the 生效规则 sub-tab).
std::string GetResolverJson() {
  std::ostringstream ss;
  ss << "{";
  ss << "\"enabled\":" << (config.IsResolverEnabled() ? "true" : "false") << ",";
  ss << "\"refresh_interval\":" << config.GetResolverRefreshInterval() << ",";
  ss << "\"max_total\":" << config.GetResolverMaxTotal() << ",";
  ss << "\"total_rules\":" << resolver::GetTotalRuleCount() << ",";

  // Subscriptions (with their cached rules for the expandable panels).
  ss << "\"subscriptions\":[";
  const auto& subs = config.GetResolverSubscriptions();
  for (size_t i = 0; i < subs.size(); ++i) {
    if (i) ss << ",";
    const auto& s = subs[i];
    ss << "{";
    ss << "\"index\":" << i << ",";
    ss << "\"name\":\"" << JsonEscape(WStringToUtf8(s.name)) << "\",";
    ss << "\"url\":\"" << JsonEscape(WStringToUtf8(s.url)) << "\",";
    ss << "\"enabled\":" << (s.enabled ? "true" : "false") << ",";
    ss << "\"last_refresh\":" << s.last_refresh << ",";
    {
      auto sub_rules_tmp = resolver::GetSubscriptionRules((int)i);
      ss << "\"rule_count\":" << (int)sub_rules_tmp.size() << ",";
    }
    ss << "\"rules\":[";
    auto sub_rules = resolver::GetSubscriptionRules((int)i);
    for (size_t j = 0; j < sub_rules.size(); ++j) {
      if (j) ss << ",";
      ss << "{\"domain\":\"" << JsonEscape(WStringToUtf8(sub_rules[j].domain))
         << "\",\"ip\":\"" << JsonEscape(WStringToUtf8(sub_rules[j].ip)) << "\"}";
    }
    ss << "]";
    ss << "}";
  }
  ss << "],";

  // Aggregated effective rules (enabled subscriptions only).
  ss << "\"rules\":[";
  auto rules = resolver::GetEffectiveRules();
  for (size_t i = 0; i < rules.size(); ++i) {
    if (i) ss << ",";
    ss << "{\"domain\":\"" << JsonEscape(WStringToUtf8(rules[i].domain))
       << "\",\"ip\":\"" << JsonEscape(WStringToUtf8(rules[i].ip))
       << "\",\"source\":\"" << JsonEscape(WStringToUtf8(rules[i].source))
       << "\"}";
  }
  ss << "]";
  ss << "}";
  return ss.str();
}

// Escape a string for embedding inside a JSON string literal. Handles the
// control characters and quotes/backslashes that Windows paths contain
// (e.g. "C:\Users" must become "C:\\Users" or JSON.parse throws).
static std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default: out += c; break;
    }
  }
  return out;
}

// Inverse of JsonEscape 鈥?revert JSON string escapes back to raw bytes. The
// config page JSON.stringify()s values, so paths arrive backslash-escaped and
// must be unescaped before they are written to the INI.
static std::string JsonUnescape(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char c = s[i + 1];
      switch (c) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        default: out += c; break;
      }
      ++i;
    } else {
      out += s[i];
    }
  }
  return out;
}

// Simple JSON string value extraction (with JSON unescaping applied)
std::string JsonGetString(const std::string& json, const std::string& key) {
  std::string pattern = "\"" + key + "\":\"";
  auto s = json.find(pattern);
  if (s == std::string::npos) return "";
  s += pattern.length();
  auto e = json.find('"', s);
  if (e == std::string::npos) return "";
  return JsonUnescape(json.substr(s, e - s));
}

bool JsonGetBool(const std::string& json, const std::string& key) {
  std::string pattern = "\"" + key + "\":";
  auto s = json.find(pattern);
  if (s == std::string::npos) return false;
  s += pattern.length();
  return json.substr(s, 4) == "true";
}

int JsonGetInt(const std::string& json, const std::string& key) {
  std::string pattern = "\"" + key + "\":";
  auto s = json.find(pattern);
  if (s == std::string::npos) return 0;
  s += pattern.length();
  char* end = nullptr;
  long val = strtol(json.substr(s).c_str(), &end, 10);
  return (end != json.substr(s).c_str()) ? (int)val : 0;
}

// --- HTTP response builder ---

std::string BuildResponse(int status, const std::string& content_type,
                          const std::string& body,
                          const std::string& extra_headers = "") {
  const char* status_text = "OK";
  switch (status) {
    case 200: status_text = "OK"; break;
    case 204: status_text = "No Content"; break;
    case 400: status_text = "Bad Request"; break;
    case 404: status_text = "Not Found"; break;
    case 500: status_text = "Internal Server Error"; break;
  }

  std::ostringstream ss;
  ss << "HTTP/1.1 " << status << " " << status_text << "\r\n";
  ss << "Content-Type: " << content_type << "\r\n";
  ss << "Content-Length: " << body.length() << "\r\n";
  ss << "Connection: close\r\n";
  ss << "Access-Control-Allow-Origin: *\r\n";
  ss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  ss << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
  ss << extra_headers;
  ss << "\r\n";
  ss << body;
  return ss.str();
}

// --- Request handler ---

std::string HandleRequest(const HttpRequest& req) {
  // Handle CORS preflight (OPTIONS) requests 鈥?must be first.
  // Browsers send OPTIONS before cross-origin POST with Content-Type: application/json.
  if (req.method == "OPTIONS") {
    return BuildResponse(204, "text/plain", "");
  }

  // Serve the Vue3 config page
  if (req.method == "GET" && (req.path == "/" || req.path == "/config" ||
                               req.path == "/index.html")) {
    // no-cache: the embedded page is rebuilt with each DLL update, so the
    // browser must always revalidate (full refetch — we have no validators)
    // instead of serving a stale heuristic-cached copy that needs Ctrl+F5.
    return BuildResponse(200, "text/html; charset=utf-8", GetWebContent(),
                         "Cache-Control: no-cache\r\n");
  }

  // API: get status
  if (req.method == "GET" && req.path == "/api/status") {
    return BuildResponse(200, "application/json", GetStatusJson());
  }

  // API: get config
  if (req.method == "GET" && req.path == "/api/config") {
    return BuildResponse(200, "application/json", GetConfigJson());
  }

  // API: update config
  if (req.method == "POST" && req.path == "/api/config") {
    std::string channel = JsonGetString(req.body, "channel");
    std::string arch = JsonGetString(req.body, "arch");
    bool auto_check = JsonGetBool(req.body, "auto_check");
    int check_interval = JsonGetInt(req.body, "check_interval");
    bool auto_download = JsonGetBool(req.body, "auto_download");
    std::string proxy = JsonGetString(req.body, "proxy");
    std::string proxy_type = JsonGetString(req.body, "proxy_type");
    bool proxy_chrome_download = JsonGetBool(req.body, "proxy_chrome_download");
    int download_source = JsonGetInt(req.body, "download_source");
    bool keep_installer = JsonGetBool(req.body, "keep_installer");
    bool keep_old_versions = JsonGetBool(req.body, "keep_old_versions");
    std::string theme = JsonGetString(req.body, "theme");
    std::string language = JsonGetString(req.body, "language");

    // --- general (portable / launch / hotkeys) ---
    std::string data_dir = JsonGetString(req.body, "data_dir");
    std::string cache_dir = JsonGetString(req.body, "cache_dir");
    std::string command_line = JsonGetString(req.body, "command_line");
    std::string launch_on_startup = JsonGetString(req.body, "launch_on_startup");
    std::string launch_on_exit = JsonGetString(req.body, "launch_on_exit");
    std::string translate_key = JsonGetString(req.body, "translate_key");
    std::string boss_key = JsonGetString(req.body, "boss_key");
    std::string open_new_window = JsonGetString(req.body, "open_new_window");
    std::string open_url_group = JsonGetString(req.body, "open_url_group");
    std::string url_group = JsonGetString(req.body, "url_group");
    bool win32k = JsonGetBool(req.body, "win32k");
    bool ignore_policies = JsonGetBool(req.body, "ignore_policies");
    bool suppress_false = JsonGetBool(req.body, "suppress_false_upgrade_notification");
    bool show_password = JsonGetBool(req.body, "show_password");
    bool debug_log = JsonGetBool(req.body, "debug_log");
    bool suppress_cmdline = JsonGetBool(req.body, "suppress_cmdline_warning");
    bool open_config_after_update = JsonGetBool(req.body, "open_config_after_update");
    bool fix_taskbar_menu = JsonGetBool(req.body, "fix_taskbar_menu");
    std::string key_mappings = JsonGetString(req.body, "key_mappings");

    // --- tabs (ported from chrome_plus tabbookmark) ---
    bool keep_last_tab = JsonGetBool(req.body, "keep_last_tab");
    bool double_click_close = JsonGetBool(req.body, "double_click_close");
    bool right_click_close = JsonGetBool(req.body, "right_click_close");
    bool wheel_tab = JsonGetBool(req.body, "wheel_tab");
    bool wheel_tab_when_press_rbutton =
        JsonGetBool(req.body, "wheel_tab_when_press_rbutton");
    bool hover_tab = JsonGetBool(req.body, "hover_tab");
    int hover_tab_delay = JsonGetInt(req.body, "hover_tab_delay");
    int open_url_new_tab = JsonGetInt(req.body, "open_url_new_tab");
    int open_bookmark_new_tab = JsonGetInt(req.body, "open_bookmark_new_tab");
    bool new_tab_disable = JsonGetBool(req.body, "new_tab_disable");
    std::string new_tab_disable_name =
        JsonGetString(req.body, "new_tab_disable_name");

    // Write appearance preferences (theme/language) to ini so they persist
    // across browser restarts, independent of the config page's localStorage.
    WritePrivateProfileStringW(L"general", L"theme",
        Utf8ToWstring(theme).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"language",
        Utf8ToWstring(language).c_str(),
        GetIniPath().c_str());

    // Write to ini file
    WritePrivateProfileStringW(L"update", L"channel",
        Utf8ToWstring(channel).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"arch",
        Utf8ToWstring(arch).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"auto_check",
        auto_check ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"check_interval",
        std::to_wstring(check_interval).c_str(), GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"auto_download",
        auto_download ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"keep_installer",
        keep_installer ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"keep_old_versions",
        keep_old_versions ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"proxy",
        Utf8ToWstring(proxy).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"proxy_type",
        Utf8ToWstring(proxy_type).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"proxy_chrome_download",
        proxy_chrome_download ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"update", L"download_source",
        std::to_wstring(download_source).c_str(), GetIniPath().c_str());

    // --- general section ---
    WritePrivateProfileStringW(L"general", L"data_dir",
        Utf8ToWstring(data_dir).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"cache_dir",
        Utf8ToWstring(cache_dir).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"command_line",
        Utf8ToWstring(command_line).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"launch_on_startup",
        Utf8ToWstring(launch_on_startup).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"launch_on_exit",
        Utf8ToWstring(launch_on_exit).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"translate_key",
        Utf8ToWstring(translate_key).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"boss_key",
        Utf8ToWstring(boss_key).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"open_new_window",
        Utf8ToWstring(open_new_window).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"open_url_group",
        Utf8ToWstring(open_url_group).c_str(),
        GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"win32k",
        win32k ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"ignore_policies",
        ignore_policies ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"suppress_false_upgrade_notification",
        suppress_false ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"show_password",
        show_password ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"debug_log",
        debug_log ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"suppress_cmdline_warning",
        suppress_cmdline ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"open_config_after_update",
        open_config_after_update ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"general", L"fix_taskbar_menu",
        fix_taskbar_menu ? L"1" : L"0", GetIniPath().c_str());

    // --- tabs section (ported from chrome_plus tabbookmark) ---
    WritePrivateProfileStringW(L"tabs", L"keep_last_tab",
        keep_last_tab ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"double_click_close",
        double_click_close ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"right_click_close",
        right_click_close ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"wheel_tab",
        wheel_tab ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"wheel_tab_when_press_rbutton",
        wheel_tab_when_press_rbutton ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"hover_tab",
        hover_tab ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"hover_tab_delay",
        std::to_wstring(hover_tab_delay).c_str(), GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"open_url_new_tab",
        std::to_wstring(open_url_new_tab).c_str(), GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"open_bookmark_new_tab",
        std::to_wstring(open_bookmark_new_tab).c_str(), GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"new_tab_disable",
        new_tab_disable ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"tabs", L"new_tab_disable_name",
        Utf8ToWstring(new_tab_disable_name).c_str(),
        GetIniPath().c_str());

    // --- keymapping section: delete then rewrite so removed entries vanish ---
    // WritePrivateProfileStringW(section, NULL, NULL) is the documented way to
    // remove an entire section. (WritePrivateProfileSectionW(NULL) is unreliable
    // at actually clearing existing keys.)
    ::WritePrivateProfileStringW(L"keymapping", nullptr, nullptr,
                                 GetIniPath().c_str());
    {
      std::string line;
      std::stringstream km_stream(key_mappings);
      while (std::getline(km_stream, line, '\n')) {
        // Trim carriage returns (CRLF) and surrounding whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t')) {
          line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
          ++start;
        }
        if (start > 0) line = line.substr(start);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string src = line.substr(0, eq);
        std::string dst = line.substr(eq + 1);
        // trim src/dst
        while (!src.empty() && (src.back() == ' ' || src.back() == '\t')) src.pop_back();
        while (!dst.empty() && (dst.front() == ' ' || dst.front() == '\t')) dst.erase(0, 1);
        if (src.empty() || dst.empty()) continue;
        WritePrivateProfileStringW(L"keymapping",
            Utf8ToWstring(src).c_str(),
            Utf8ToWstring(dst).c_str(),
            GetIniPath().c_str());
      }
    }

    // --- url_group section: delete then rewrite so removed entries vanish ---
    ::WritePrivateProfileStringW(L"url_group", nullptr, nullptr,
                                 GetIniPath().c_str());
    {
      std::string line;
      std::stringstream ug_stream(url_group);
      int idx = 1;
      while (std::getline(ug_stream, line, '\n')) {
        // Trim carriage returns (CRLF) and surrounding whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t')) {
          line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
          ++start;
        }
        if (start > 0) line = line.substr(start);
        if (line.empty()) continue;
        WritePrivateProfileStringW(L"url_group",
            std::to_wstring(idx).c_str(),
            Utf8ToWstring(line).c_str(),
            GetIniPath().c_str());
        ++idx;
      }
    }

    // Reload the singleton from ini so the just-written values are visible to
    // the next GET /api/config (otherwise GetConfigJson reads stale values and
    // the frontend's refreshConfig() overwrites the user's change immediately).
    Config::Instance().ReloadConfig();

    // Update global state
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.channel = ParseChannel(
          Utf8ToWstring(channel));
      g_update_state.auto_check = auto_check;
      g_update_state.auto_download = auto_download;
      g_update_state.proxy = proxy;
      g_update_state.proxy_type = proxy_type;
      g_update_state.proxy_chrome_download = proxy_chrome_download;
      g_update_state.download_source = download_source;
    }
    SaveUpdateState();

    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: trigger update check
  if (req.method == "POST" && req.path == "/api/check") {
    TriggerUpdateCheck();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: trigger download
  if (req.method == "POST" && req.path == "/api/download") {
    TriggerDownload();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: offline install 鈥?use local .7z from updates folder if available.
  // TriggerDownload() will detect the local package and skip download.
  // Can be called from any state (even idle) for offline installation.
  if (req.method == "POST" && req.path == "/api/install-offline") {
    if (!HasLocalPackage()) {
      return BuildResponse(400, "application/json",
                           "{\"error\":\"No local package found in updates/ folder\"}");
    }
    TriggerDownload();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: cancel download
  if (req.method == "POST" && req.path == "/api/cancel") {
    Downloader::Instance().Cancel();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: reset update state to idle (clear stuck ready/available/error)
  if (req.method == "POST" && req.path == "/api/reset") {
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.state = UpdateState::kIdle;
      g_update_state.download_progress = 0;
      g_update_state.downloaded_bytes = 0;
      g_update_state.download_speed = 0;
      g_update_state.download_eta = 0;
      g_update_state.download_path.clear();
      g_update_state.sha256.clear();
      g_update_state.error_message.clear();
    }
    SaveUpdateState();

    // Also clear the update working folders (updates/ + update_temp/) so a
    // reset after a failed/interrupted download removes the broken partial
    // installer. The download runs on a thread INSIDE this chrome.exe
    // process and holds the output file open with an exclusive handle —
    // that is why manual deletion in Explorer fails with "file is open in
    // Chrome". Cancel first (CancelForReset also suppresses the download
    // thread's exit-path state transition so it doesn't overwrite the kIdle
    // set above), wait for the thread to close its file handle (at worst
    // when the blocked WinHTTP read times out), then delete the folders.
    // The wait can take up to ~a minute, so run it in the background — the
    // state is already reset, the folder deletion is just cleanup.
    Downloader::Instance().CancelForReset();
    std::thread([]() {
      Downloader::Instance().Wait();
      CleanUpdateWorkDirs();
    }).detach();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: apply update (mark for next restart)
  if (req.method == "POST" && req.path == "/api/apply") {
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.state = UpdateState::kReady;
    }
    SaveUpdateState();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: apply update and restart chrome
  if (req.method == "POST" && req.path == "/api/restart") {
    DiagLog(L"[HTTP] /api/restart received");
    // First, extract the Chrome update archive to temp dir (if pending).
    // ApplyPendingUpdate may change state from kReady to kPendingApply.
    ApplyPendingUpdate();

    // Take snapshot AFTER ApplyPendingUpdate 鈥?the state may have changed.
    auto state = GetUpdateStateSnapshot();
    DiagLog(L"[HTTP] /api/restart: after ApplyPendingUpdate state={} self_update_ready={}",
            static_cast<int>(state.state), state.self_update_ready ? 1 : 0);

    // Use the bat script approach for ALL restarts (Chrome update and/or self-update).
    // The bat script: waits for this process to exit 鈫?moves Chrome files (if pending)
    // 鈫?copies new DLL (if self-update) 鈫?cleans up 鈫?restarts Chrome 鈫?self-deletes.
    // This avoids the flash-and-close bug caused by ShellExecuteW launching new Chrome
    // before the old one has fully exited (old chrome.exe memory mapping conflicts).
    bool has_update_action = (state.state == UpdateState::kPendingApply ||
                              state.self_update_ready ||
                              PathFileExistsW((GetAppDir() + L"\\chrome.exe.new").c_str()));

    if (has_update_action) {
      // Launch the standalone GUI update window. It waits for this Chrome to
      // exit, applies the update, then relaunches Chrome.
      if (LaunchUpdateWindow()) {
        DiagLog(L"[HTTP] /api/restart: LaunchUpdateWindow OK (update window launched)");
      } else {
        AddDebugLog("Update restart (window) failed, restarting without update actions");
        DiagLog(L"[HTTP] /api/restart: LaunchUpdateWindow FAILED -> plain restart");
        has_update_action = false;
      }
      // On a successful update action, reset self-update state before exiting
      // so we don't re-trigger it next launch.
      if (has_update_action) {
        if (state.self_update_ready) {
          std::lock_guard<std::mutex> lock(g_update_mutex);
          g_update_state.self_update_ready = false;
          g_update_state.self_self_download_progress = 0;
          g_update_state.self_download_path.clear();
        }
        SaveUpdateState();
      }
    }

    // Schedule a restart after sending the response
    std::thread([has_update_action]() {
      Sleep(500);
      if (has_update_action) {
        // The update window handles restart after update 鈥?just exit
        ExitProcess(0);
      } else {
        // Normal restart: launch new Chrome, then exit this one
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        ShellExecuteW(nullptr, L"open", exe_path, nullptr, nullptr, SW_SHOWNORMAL);
        ExitProcess(0);
      }
    DiagLog(L"[HTTP] /api/restart: responding ok, has_update_action={} (detached thread exits Chrome in 500ms)",
            has_update_action ? 1 : 0);
    }).detach();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: check for chrome_green self-update (Release channel only)
  if (req.method == "GET" && req.path.find("/api/self-update/check") == 0) {
    auto state = GetUpdateStateSnapshot();
    UpdateInfo info = CheckSelfUpdate(state.proxy, state.proxy_type);
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      // self_latest_version always records the newest published release (for
      // display), but the "has update" decision is the backend-derived flag.
      g_update_state.self_latest_version = info.version;
      g_update_state.self_has_update = info.has_update;
      if (info.has_update) {
        g_update_state.self_download_url = info.urls.empty() ? "" : info.urls[0];
        g_update_state.self_download_size = info.size;
        g_update_state.self_release_notes = info.sha1;
      } else {
        g_update_state.self_download_url = "";
        g_update_state.self_download_size = 0;
        g_update_state.self_release_notes = "";
      }
    }

    std::ostringstream ss;
    ss << "{";
    ss << "\"has_update\":" << (info.has_update ? "true" : "false") << ",";
    ss << "\"self_has_update\":" << (info.has_update ? "true" : "false") << ",";
    ss << "\"version\":\"" << info.version << "\",";
    ss << "\"size\":" << info.size << ",";
    ss << "\"url\":\"" << (info.urls.empty() ? "" : info.urls[0]) << "\",";
    ss << "\"release_notes\":\"" << info.sha1 << "\"";
    ss << "}";
    return BuildResponse(200, "application/json", ss.str());
  }

  // API: download chrome_green self-update
  if (req.method == "POST" && req.path == "/api/self-update/download") {
    auto state = GetUpdateStateSnapshot();
    if (state.self_download_url.empty()) {
      return BuildResponse(400, "application/json", "{\"error\":\"No download URL\"}");
    }

    // Build save path: <dll dir>/updates/ChromeGreen_vX.Y.Z.zip
    std::wstring data_dir = GetSelfDllDir() + L"\\updates";
    CreateDirectoryW(data_dir.c_str(), nullptr);
    std::wstring save_path = data_dir + L"\\chrome_green_self_update.zip";
    std::wstring extract_dir = data_dir + L"\\self_update";

    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.self_download_path = "updates\\self_update\\version.dll";
      g_update_state.self_self_download_progress = 0;
      g_update_state.self_update_ready = false;
      g_update_state.self_downloading = true;
    }

    // Download + extract in a background thread
    std::thread([url = state.self_download_url, save_path, extract_dir,
                 proxy = state.proxy, proxy_type = state.proxy_type]() {
      // Clean any previous extraction
      RemoveDirectoryRecursiveLocal(extract_dir);
      bool ok = DownloadSelfUpdate(url, save_path, extract_dir, proxy, proxy_type);
      if (ok) {
        std::lock_guard<std::mutex> lock(g_update_mutex);
        g_update_state.self_update_ready = true;
        g_update_state.self_self_download_progress = 100;
        g_update_state.self_download_path = "updates\\self_update\\version.dll";
        g_update_state.self_downloading = false;
        g_update_state.error_message.clear();
      } else {
        std::lock_guard<std::mutex> lock(g_update_mutex);
        g_update_state.self_update_ready = false;
        g_update_state.self_self_download_progress = 0;
        g_update_state.self_downloading = false;
        g_update_state.error_message = "Failed to download or extract self-update";
      }
      SaveUpdateState();
    }).detach();

    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: apply self-update (swap version.dll via hidden bat script + restart)
  if (req.method == "POST" && req.path == "/api/self-update/apply") {
    DiagLog(L"[HTTP] /api/self-update/apply received");
    auto state = GetUpdateStateSnapshot();
    if (!state.self_update_ready) {
      DiagLog(L"[HTTP] /api/self-update/apply: no self-update ready, rejecting");
      return BuildResponse(400, "application/json", "{\"error\":\"No self-update ready\"}");
    }

    // Also apply Chrome update if pending (combined restart)
    ApplyPendingUpdate();

    // Launch the update action via the standalone GUI window. It will wait for
    // this Chrome process to exit 鈫?move Chrome update files (if pending) 鈫?
    // copy new version.dll over old one 鈫?restart Chrome 鈫?self-remove.
    if (!LaunchUpdateWindow()) {
      DiagLog(L"[HTTP] /api/self-update/apply: LaunchUpdateWindow FAILED");
      return BuildResponse(500, "application/json", "{\"error\":\"Failed to launch update\"}");
    }
    DiagLog(L"[HTTP] /api/self-update/apply: LaunchUpdateWindow OK");

    // Reset self-update state before exiting
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.self_update_ready = false;
      g_update_state.self_self_download_progress = 0;
      g_update_state.self_download_path.clear();
      g_update_state.self_has_update = false;
      g_update_state.self_latest_version = "";
    }
    SaveUpdateState();

    // Exit Chrome immediately 鈥?the update window will restart it after the DLL swap
    std::thread([]() {
      Sleep(300);
      ExitProcess(0);
    }).detach();

    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: clear debug logs
  if (req.method == "POST" && req.path == "/api/logs/clear") {
    ClearDebugLogs();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: get debug logs
  if (req.method == "GET" && req.path == "/api/logs") {
    auto logs = GetDebugLogs();
    std::ostringstream ss;
    ss << "{\"logs\":[";
    for (size_t i = 0; i < logs.size(); i++) {
      if (i > 0) ss << ",";
      ss << "\"" << JsonEscape(logs[i]) << "\"";
    }
    ss << "]}";
    return BuildResponse(200, "application/json", ss.str());
  }

  // API: tools status (detection for config-page indicators)
  if (req.method == "GET" && req.path == "/api/tools/status") {
    std::ostringstream ss;
    ss << "{";
    ss << "\"desktop_shortcut\":" << (DesktopShortcutExists() ? "true" : "false");
    ss << "}";
    return BuildResponse(200, "application/json", ss.str());
  }

  // API: create desktop shortcut to the portable Chrome
  if (req.method == "POST" && req.path == "/api/tools/desktop-shortcut") {
    auto r = CreateDesktopShortcut();
    std::ostringstream ss;
    ss << "{";
    ss << "\"success\":" << (r.success ? "true" : "false") << ",";
    ss << "\"done\":" << (r.done ? "true" : "false") << ",";
    ss << "\"message\":\"" << JsonEscape(ToUtf8(r.message)) << "\"";
    ss << "}";
    return BuildResponse(200, "application/json", ss.str());
  }

  // API: status of the default (non-portable) Chrome data directory
  if (req.method == "GET" && req.path == "/api/tools/chrome-data-status") {
    auto s = GetChromeDefaultDataStatus();
    std::ostringstream ss;
    ss << "{";
    ss << "\"exists\":" << (s.exists ? "true" : "false") << ",";
    ss << "\"empty\":" << (s.empty ? "true" : "false") << ",";
    ss << "\"entry_count\":" << s.entry_count;
    ss << "}";
    return BuildResponse(200, "application/json", ss.str());
  }

  // API: clean the default Chrome data directory
  if (req.method == "POST" && req.path == "/api/tools/clean-chrome-data") {
    auto r = CleanChromeDefaultData();
    std::ostringstream ss;
    ss << "{";
    ss << "\"success\":" << (r.success ? "true" : "false") << ",";
    ss << "\"message\":\"" << JsonEscape(ToUtf8(r.message)) << "\"";
    ss << "}";
    return BuildResponse(200, "application/json", ss.str());
  }

  // API: begin backend-assisted hotkey capture (swallows Chrome shortcuts).
  if (req.method == "POST" && req.path == "/api/capture/start") {
    StartKeyCapture();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: abort an in-progress hotkey capture.
  if (req.method == "POST" && req.path == "/api/capture/stop") {
    StopKeyCapture();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // ===== Domain-mapping (域名映射) API =====

  // API: get resolver config + subscriptions + effective rules.
  if (req.method == "GET" && req.path == "/api/resolver") {
    return BuildResponse(200, "application/json", GetResolverJson());
  }

  // API: set global resolver config (enabled / refresh_interval / max_total).
  if (req.method == "POST" && req.path == "/api/resolver/config") {
    bool enabled = JsonGetBool(req.body, "enabled");
    int refresh_interval = JsonGetInt(req.body, "refresh_interval");
    int max_total = JsonGetInt(req.body, "max_total");
    if (max_total <= 0) max_total = 800;
    WritePrivateProfileStringW(L"resolver_rules", L"enabled",
        enabled ? L"1" : L"0", GetIniPath().c_str());
    WritePrivateProfileStringW(L"resolver_rules", L"refresh_interval",
        std::to_wstring(refresh_interval).c_str(), GetIniPath().c_str());
    WritePrivateProfileStringW(L"resolver_rules", L"max_total",
        std::to_wstring(max_total).c_str(), GetIniPath().c_str());
    Config::Instance().ReloadConfig();
    return BuildResponse(200, "application/json", "{\"ok\":true}");
  }

  // API: add a subscription (download + parse + enforce total cap).
  if (req.method == "POST" && req.path == "/api/resolver/add") {
    std::wstring name = Utf8ToWstring(JsonGetString(req.body, "name"));
    std::wstring url = Utf8ToWstring(JsonGetString(req.body, "url"));
    std::wstring error;
    bool ok = resolver::AddSubscription(name, url, error);
    std::ostringstream ss;
    ss << "{\"ok\":" << (ok ? "true" : "false") << ",";
    ss << "\"error\":\"" << JsonEscape(WStringToUtf8(error)) << "\"}";
    return BuildResponse(ok ? 200 : 400, "application/json", ss.str());
  }

  // API: remove a subscription by index.
  if (req.method == "POST" && req.path == "/api/resolver/remove") {
    int index = JsonGetInt(req.body, "index");
    bool ok = resolver::RemoveSubscription(index);
    return BuildResponse(ok ? 200 : 400, "application/json",
        ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"订阅不存在\"}");
  }

  // API: enable/disable a subscription.
  if (req.method == "POST" && req.path == "/api/resolver/enable") {
    int index = JsonGetInt(req.body, "index");
    bool enabled = JsonGetBool(req.body, "enabled");
    bool ok = resolver::SetSubscriptionEnabled(index, enabled);
    return BuildResponse(ok ? 200 : 400, "application/json",
        ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"订阅不存在\"}");
  }

  // API: refresh a subscription (index>=0) or all enabled subscriptions
  // (index<0). On overflow the old cache is kept and an error is returned.
  if (req.method == "POST" && req.path == "/api/resolver/refresh") {
    int index = JsonGetInt(req.body, "index");
    std::wstring error;
    bool ok = true;
    if (index < 0) {
      const auto& subs = config.GetResolverSubscriptions();
      for (size_t i = 0; i < subs.size(); ++i) {
        if (!subs[i].enabled) continue;
        std::wstring e;
        if (!resolver::RefreshSubscription((int)i, e)) {
          ok = false;
          if (!error.empty()) error += L"; ";
          error += subs[i].name + L": " + e;
        }
      }
    } else {
      ok = resolver::RefreshSubscription(index, error);
    }
    std::ostringstream ss;
    ss << "{\"ok\":" << (ok ? "true" : "false") << ",";
    ss << "\"error\":\"" << JsonEscape(WStringToUtf8(error)) << "\"}";
    return BuildResponse(ok ? 200 : 400, "application/json", ss.str());
  }

  // API: export the effective (enabled) rules as a hosts-format file.
  if (req.method == "POST" && req.path == "/api/resolver/export") {
    std::wstring error;
    bool ok = resolver::ExportRules("", error);
    std::ostringstream ss;
    ss << "{\"ok\":" << (ok ? "true" : "false") << ",";
    ss << "\"error\":\"" << JsonEscape(WStringToUtf8(error)) << "\"}";
    return BuildResponse(ok ? 200 : 400, "application/json", ss.str());
  }

  // API: update a subscription's name and URL in place by index.
  if (req.method == "POST" && req.path == "/api/resolver/update") {
    int index = JsonGetInt(req.body, "index");
    std::wstring name = Utf8ToWstring(JsonGetString(req.body, "name"));
    std::wstring url = Utf8ToWstring(JsonGetString(req.body, "url"));
    std::wstring error;
    bool ok = resolver::UpdateSubscription(index, name, url, error);
    std::ostringstream ss;
    ss << "{\"ok\":" << (ok ? "true" : "false") << ",";
    ss << "\"error\":\"" << JsonEscape(WStringToUtf8(error)) << "\"}";
    return BuildResponse(ok ? 200 : 400, "application/json", ss.str());
  }

  return BuildResponse(404, "text/plain", "Not Found");
}

// --- Server thread ---

// Send a complete HTTP response and close the socket. Shared by all one-shot
// request handlers.
void SendResponse(SOCKET client, const std::string& response) {
  size_t total_sent = 0;
  while (total_sent < response.length()) {
    int sent = send(client, response.c_str() + total_sent,
                    (int)(response.length() - total_sent), 0);
    if (sent == SOCKET_ERROR) break;
    total_sent += sent;
  }
  shutdown(client, SD_BOTH);
  closesocket(client);
}

// Server-Sent Events handler: keep the connection open and stream the status
// JSON (~every 300ms) so the config page updates the progress bar / speed /
// ETA in real time without any client-side polling. EventSource auto-reconnects
// on a dropped connection, so transient disconnects are harmless. The stream
// ends when the client disconnects or the server stops; either way we close.
void HandleStream(SOCKET client) {
  // A dead/absent reader must cause send() to fail (not block forever), so we
  // keep a modest send timeout to detect disconnects via the return value.
  DWORD sndtimeo = 2000;
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
             (const char*)&sndtimeo, sizeof(sndtimeo));

  std::string headers =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache, no-transform\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "X-Accel-Buffering: no\r\n"
      "retry: 1000\r\n"
      "\r\n";
  if (send(client, headers.c_str(), (int)headers.length(), 0) == SOCKET_ERROR) {
    shutdown(client, SD_BOTH);
    closesocket(client);
    return;
  }

  auto push = [&]() -> bool {
    std::string frame = "data: " + GetStatusJson() + "\n\n";
    return send(client, frame.c_str(), (int)frame.length(), 0) != SOCKET_ERROR;
  };

  // Flush any pending one-shot key-capture event as a named SSE event so the
  // frontend's EventSource listener (addEventListener('key_capture')) picks it
  // up without waiting for the next status frame.
  auto pushCapture = [&]() -> bool {
    std::string payload;
    if (!ConsumeKeyCaptureEvent(payload)) return true;
    std::string frame = "event: key_capture\ndata: " + payload + "\n\n";
    return send(client, frame.c_str(), (int)frame.length(), 0) != SOCKET_ERROR;
  };

  // Immediate first frame, then stream until disconnect / server stop.
  if (!push() || !pushCapture()) {
    shutdown(client, SD_BOTH);
    closesocket(client);
    return;
  }
  while (server_running_.load()) {
    Sleep(300);
    if (!push()) break;
    if (!pushCapture()) break;
  }

  shutdown(client, SD_BOTH);
  closesocket(client);
}

// Each request runs on its own detached thread, so a slow or half-open
// connection can't stall the others.
void HandleClient(SOCKET client) {
  // Set socket options BEFORE any I/O
  // TCP_NODELAY: disable Nagle so the small HTML headers flush immediately
  BOOL nodelay = TRUE;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
             (const char*)&nodelay, sizeof(nodelay));

  // SO_RCVTIMEO: 5s timeout on recv so a half-open client can't block us
  DWORD rcvtimeo = 5000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
             (const char*)&rcvtimeo, sizeof(rcvtimeo));

  // SO_SNDTIMEO: 5s timeout on send
  DWORD sndtimeo = 5000;
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
             (const char*)&sndtimeo, sizeof(sndtimeo));

  // Read request (simple: read until we have headers + body)
  std::string raw_request;
  char buf[4096];
  bool headers_complete = false;
  int total_received = 0;

  while (total_received < 1024 * 1024) {  // 1MB max
    int received = recv(client, buf, sizeof(buf), 0);
    if (received <= 0) break;

    raw_request.append(buf, received);
    total_received += received;

    if (!headers_complete) {
      if (raw_request.find("\r\n\r\n") != std::string::npos) {
        headers_complete = true;
        // Check Content-Length for POST body
        std::string lower = raw_request;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto cl_pos = lower.find("content-length:");
        if (cl_pos != std::string::npos) {
          auto line_end = lower.find("\r\n", cl_pos);
          char* end = nullptr;
          long cl_val = strtol(lower.substr(cl_pos + 15,
              line_end - cl_pos - 15).c_str(), &end, 10);
          int content_length = (int)cl_val;
          auto header_end = raw_request.find("\r\n\r\n");
          int body_received = total_received - header_end - 4;
          if (body_received < content_length) continue;
        }
        break;  // No body needed or body complete
      }
    }
  }

  // Parse and handle
  HttpRequest req;
  if (!ParseRequest(raw_request, req)) {
    SendResponse(client, BuildResponse(400, "text/plain", "Bad Request"));
    return;
  }

  // --- Server-Sent Events: stream live status without polling ---
  // The config page opens a single EventSource('/api/stream') connection that
  // stays open; we push the status JSON every ~300ms so the progress bar /
  // speed / ETA update in real time. Returning here skips the one-shot path.
  if (req.method == "GET" && req.path == "/api/stream") {
    HandleStream(client);
    return;
  }

  SendResponse(client, HandleRequest(req));
}

void ServerThread() {
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    server_running_.store(false);
    return;
  }

  // If reservation didn't bind a socket (skipped or every port busy), fall
  // back to binding the per-install base port directly (legacy behavior).
  if (g_listen_socket == INVALID_SOCKET) {
    int port = GetConfigPort();
    g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen_socket == INVALID_SOCKET) {
      WSACleanup();
      server_running_.store(false);
      return;
    }
    BOOL opt = TRUE;
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)port);
    if (bind(g_listen_socket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
      closesocket(g_listen_socket);
      g_listen_socket = INVALID_SOCKET;
      WSACleanup();
      server_running_.store(false);
      return;
    }
    g_config_port = port;
  }

  if (listen(g_listen_socket, 16) == SOCKET_ERROR) {
    closesocket(g_listen_socket);
    g_listen_socket = INVALID_SOCKET;
    WSACleanup();
    server_running_.store(false);
    return;
  }

  DebugLog(L"HTTP server listening on port {}", g_config_port);

  // Persist the actual bound port so the updater can open the config page
  // after an in-app update. The port is derived from the install dir but may
  // be scan-shifted if the base was occupied, so record the real value here.
  WritePrivateProfileStringW(L"general", L"config_port",
      std::to_wstring(g_config_port).c_str(), GetIniPath().c_str());

  while (server_running_.load()) {
    // Set a timeout so we can check server_running_ periodically
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(g_listen_socket, &read_set);
    timeval timeout = {1, 0};  // 1 second

    int select_result = select(0, &read_set, nullptr, nullptr, &timeout);
    if (select_result <= 0) continue;

    SOCKET client = accept(g_listen_socket, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;

    // Spawn a detached thread so a single slow/half-open connection
    // never blocks other requests. HandleClient owns the socket lifetime.
    std::thread(HandleClient, client).detach();
  }

  closesocket(g_listen_socket);
  g_listen_socket = INVALID_SOCKET;
  WSACleanup();
  DebugLog(L"HTTP server stopped");
}

}  // namespace

int ReserveConfigServerPort() {
  if (g_config_port != 0) return g_config_port;  // already reserved
  int base = GetConfigPort();
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    g_config_port = base;
    return base;
  }
  for (int i = 0; i < 50; ++i) {
    int p = base + i;
    if (p > 65535) p = 1024 + ((p - 65535 - 1) % (65535 - 1024 + 1));
    if (p < 1024) p = 1024;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) continue;
    BOOL opt = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)p);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
      g_listen_socket = s;  // held until ServerThread takes over (listen)
      g_config_port = p;
      return p;
    }
    closesocket(s);
  }
  // Every candidate occupied: fall back to the base so baked links stay
  // consistent (the server will then likely fail to bind, but the URL still
  // points at a deterministic port).
  g_config_port = base;
  return base;
}

int GetConfigServerPort() {
  return g_config_port != 0 ? g_config_port : GetConfigPort();
}

void StartHttpServer() {
  if (server_running_.load()) return;

  server_running_.store(true);
  server_thread_ = std::thread(ServerThread);
}

void StopHttpServer() {
  server_running_.store(false);
  if (server_thread_.joinable()) {
    server_thread_.join();
  }
}

bool IsHttpServerRunning() {
  return server_running_.load();
}
