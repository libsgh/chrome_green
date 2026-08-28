#include "hosts_manager.h"

#include <atomic>
#include <ctime>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <shlobj.h>  // SHCreateDirectoryExW (nested dir creation)

#include "config.h"
#include "utils.h"

namespace resolver {

namespace {

bool IsValidIpv4(const std::wstring& s) {
  auto parts = StringSplit(s, L'.');
  if (parts.size() != 4) return false;
  for (const auto& p : parts) {
    if (p.empty() || p.size() > 3) return false;
    for (wchar_t c : p)
      if (c < L'0' || c > L'9') return false;
    int v = 0;
    for (wchar_t c : p) v = v * 10 + (c - L'0');
    if (v > 255) return false;
  }
  return true;
}

bool IsValidIpv6(const std::wstring& s) {
  if (s.size() < 3) return false;
  size_t first = s.find(L':');
  if (first == std::wstring::npos) return false;
  if (s.find(L':', first + 1) == std::wstring::npos) return false;
  for (wchar_t c : s) {
    if (c == L':') continue;
    if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
        (c >= L'A' && c <= L'F'))
      continue;
    return false;
  }
  return true;
}

bool IsValidIp(const std::wstring& s) {
  if (s.empty()) return false;
  if (s.find(L':') != std::wstring::npos) return IsValidIpv6(s);
  return IsValidIpv4(s);
}

bool IsValidDomain(const std::wstring& s) {
  if (s.empty() || s.size() > 253) return false;
  for (wchar_t c : s) {
    if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
        (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'*' ||
        c == L'_')
      continue;
    return false;
  }
  return true;
}

// HTTP GET a URL as UTF-8 text, following up to 5 redirects.
bool WinHttpGetString(const std::string& url, std::string& out,
                      std::wstring& error) {
  out.clear();
  std::string current = url;
  for (int redirect = 0; redirect < 6; ++redirect) {
    bool https = true;
    std::string path = current;
    if (current.rfind("https://", 0) == 0) {
      path = current.substr(8);
      https = true;
    } else if (current.rfind("http://", 0) == 0) {
      path = current.substr(7);
      https = false;
    } else {
      error = L"不支持的 URL（仅支持 http/https）";
      return false;
    }

    std::string host = path;
    auto slash = path.find('/');
    if (slash != std::string::npos) {
      host = path.substr(0, slash);
      path = path.substr(slash);
    } else {
      path = "/";
    }

    int port = https ? 443 : 80;
    auto colon = host.rfind(':');
    if (colon != std::string::npos) {
      errno = 0;
      char* endp = nullptr;
      long v = strtol(host.c_str() + colon + 1, &endp, 10);
      if (endp != host.c_str() + colon + 1 && *endp == '\0' && v > 0 &&
          v <= 65535) {
        port = (int)v;
        host = host.substr(0, colon);
      }
    }

    std::wstring host_w = Utf8ToWstring(host);
    std::wstring path_w = Utf8ToWstring(path);

    HINTERNET hSession =
        WinHttpOpen(L"ChromeGreen Resolver", WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
      error = L"WinHttpOpen 失败";
      return false;
    }
    HINTERNET hConnect =
        WinHttpConnect(hSession, host_w.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
      WinHttpCloseHandle(hSession);
      error = L"连接失败";
      return false;
    }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path_w.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      error = L"构造请求失败";
      return false;
    }
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      error = L"发送请求失败";
      return false;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      error = L"接收响应失败";
      return false;
    }

    // Follow redirects.
    DWORD status = 0, sl = sizeof(status);
    if (WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
                            nullptr)) {
      if (status == 301 || status == 302 || status == 303 || status == 307 ||
          status == 308) {
        DWORD ls = 0;
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &ls,
                                nullptr) &&
            ls > 0) {
          std::string loc(ls + 1, 0);
          if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                  WINHTTP_HEADER_NAME_BY_INDEX,
                                  (LPVOID)loc.data(), &ls, nullptr)) {
            loc.resize(ls);
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (loc.rfind("http", 0) == 0) {
              current = loc;
            } else if (!loc.empty() && loc[0] == '/') {
              current = (https ? "https://" : "http://") + host + loc;
            } else {
              current = (https ? "https://" : "http://") + host + "/" + loc;
            }
            continue;
          }
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        error = L"重定向缺少 Location 头";
        return false;
      }
    }

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
      std::vector<char> buf(avail);
      DWORD read = 0;
      if (WinHttpReadData(hRequest, buf.data(), avail, &read) && read > 0)
        out.append(buf.data(), read);
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
  }
  error = L"重定向次数过多";
  return false;
}

std::wstring CacheFilePath(const std::wstring& base) {
  return GetResolverCacheDir() + L"\\" + base + L".txt";
}

bool WriteCacheFile(const std::wstring& base,
                    const std::vector<ResolvedRule>& rules) {
  std::wstring path = CacheFilePath(base);
  std::string text = RulesToHostsText(rules);
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
  CloseHandle(h);
  return true;
}

bool ReadCacheFile(const std::wstring& base, std::vector<ResolvedRule>& out) {
  std::wstring path = CacheFilePath(base);
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    return false;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  std::string text;
  char buf[4096];
  DWORD read = 0;
  while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0)
    text.append(buf, read);
  CloseHandle(h);
  int invalid = 0;
  ParseHostsText(text, out, invalid);
  return true;
}

int NextSubIndex() {
  int maxn = 0;
  for (const auto& sub : config.GetResolverSubscriptions()) {
    if (sub.cache.rfind(L"sub_", 0) == 0) {
      int n = (int)wcstoll(sub.cache.substr(4).c_str(), nullptr, 10);
      if (n > maxn) maxn = n;
    }
  }
  return maxn + 1;
}

long long NowUnix() { return (long long)time(nullptr); }

void WriteSubInt(int index, const std::wstring& suffix, long long value) {
  std::wstring key = L"sub_" + std::to_wstring(index + 1) + suffix;
  WritePrivateProfileStringW(L"resolver_rules", key.c_str(),
                              std::to_wstring(value).c_str(),
                              GetIniPath().c_str());
}

}  // namespace

bool ParseHostsText(const std::string& text, std::vector<ResolvedRule>& out,
                    int& invalid_lines) {
  invalid_lines = 0;
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    size_t s = 0, e = line.size();
    while (s < e && (line[s] == ' ' || line[s] == '\t')) ++s;
    while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
    if (s >= e) continue;
    line = line.substr(s, e - s);

    std::vector<std::string> tok;
    size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
      if (i >= line.size()) break;
      size_t j = i;
      while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
      tok.push_back(line.substr(i, j - i));
      i = j;
    }
    if (tok.size() < 2) {
      ++invalid_lines;
      continue;
    }
    std::wstring ip = Utf8ToWstring(tok[0]);
    if (!IsValidIp(ip)) {
      ++invalid_lines;
      continue;
    }
    for (size_t k = 1; k < tok.size(); ++k) {
      std::wstring dom = Utf8ToWstring(tok[k]);
      if (!IsValidDomain(dom)) {
        ++invalid_lines;
        continue;
      }
      ResolvedRule r;
      r.ip = ip;
      r.domain = dom;
      out.push_back(std::move(r));
    }
  }
  return !out.empty();
}

std::string RulesToMapBody(const std::vector<ResolvedRule>& rules) {
  std::string body;
  for (const auto& r : rules) {
    if (!body.empty()) body += ',';
    body += "MAP ";
    body += WStringToUtf8(r.domain);
    body += ' ';
    body += WStringToUtf8(r.ip);
  }
  return body;
}

std::string RulesToHostsText(const std::vector<ResolvedRule>& rules) {
  std::string text;
  for (const auto& r : rules) {
    text += WStringToUtf8(r.ip);
    text += ' ';
    text += WStringToUtf8(r.domain);
    text += '\n';
  }
  return text;
}

std::wstring GetResolverCacheDir() {
  std::wstring dir = GetAbsolutePath(
      CanonicalizePath(GetAppDir() + L"\\..\\Data\\resolver_cache"));
  SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  return dir;
}

std::wstring BuildResolverRulesSwitch() {
  if (!config.IsResolverEnabled()) return L"";
  std::vector<ResolvedRule> all = GetEffectiveRules();
  if (all.empty()) return L"";
  // Do NOT add internal quotes around the value.  JoinArgsString() in
  // portable.cc will wrap the whole switch in outer quotes when it contains
  // spaces, which is the same quoting style used for --user-data-dir etc.
  // Internal quotes would be doubled and can confuse Chromium's parser.
  std::wstring sw = L"--host-resolver-rules=";
  sw += Utf8ToWstring(RulesToMapBody(all));
  return sw;
}

std::vector<ResolvedRule> GetEffectiveRules() {
  std::vector<ResolvedRule> out;
  for (const auto& sub : config.GetResolverSubscriptions()) {
    if (!sub.enabled) continue;
    std::vector<ResolvedRule> rules;
    if (!ReadCacheFile(sub.cache, rules)) continue;
    for (auto& r : rules) r.source = sub.name;
    out.insert(out.end(), rules.begin(), rules.end());
  }
  return out;
}

int GetTotalRuleCount() {
  int total = 0;
  for (const auto& sub : config.GetResolverSubscriptions())
    if (sub.enabled) total += sub.rule_count;
  return total;
}

std::vector<ResolvedRule> GetSubscriptionRules(int index) {
  std::vector<ResolvedRule> out;
  const auto& subs = config.GetResolverSubscriptions();
  if (index < 0 || index >= (int)subs.size()) return out;
  ReadCacheFile(subs[index].cache, out);
  for (auto& r : out) r.source = subs[index].name;
  return out;
}

bool HttpGetText(const std::string& url, std::string& out,
                 std::wstring& error) {
  return WinHttpGetString(url, out, error);
}

bool AddSubscription(const std::wstring& name, const std::wstring& url,
                     std::wstring& error) {
  if (name.empty() || url.empty()) {
    error = L"名称和 URL 不能为空";
    return false;
  }
  std::string text;
  if (!WinHttpGetString(WStringToUtf8(url), text, error)) return false;

  std::vector<ResolvedRule> rules;
  int invalid = 0;
  ParseHostsText(text, rules, invalid);
  if (rules.empty()) {
    error = L"订阅内容为空或无可解析规则（" + std::to_wstring(invalid) +
           L" 行无效）";
    return false;
  }

  int max_total = config.GetResolverMaxTotal();
  int prospective = GetTotalRuleCount() + (int)rules.size();
  if (prospective > max_total) {
    error = L"添加失败：规则总数将达 " + std::to_wstring(prospective) +
           L"，超出上限 " + std::to_wstring(max_total);
    return false;
  }

  int n = NextSubIndex();
  std::wstring base = L"sub_" + std::to_wstring(n);
  if (!WriteCacheFile(base, rules)) {
    error = L"写入缓存失败";
    return false;
  }

  std::wstring idx = std::to_wstring(n);
  WritePrivateProfileStringW(L"resolver_rules",
                              (L"sub_" + idx + L"_name").c_str(), name.c_str(),
                              GetIniPath().c_str());
  WritePrivateProfileStringW(L"resolver_rules",
                              (L"sub_" + idx + L"_url").c_str(), url.c_str(),
                              GetIniPath().c_str());
  WritePrivateProfileStringW(L"resolver_rules",
                              (L"sub_" + idx + L"_enabled").c_str(), L"1",
                              GetIniPath().c_str());
  WritePrivateProfileStringW(L"resolver_rules",
                              (L"sub_" + idx + L"_last_refresh").c_str(),
                              std::to_wstring(NowUnix()).c_str(),
                              GetIniPath().c_str());
  WritePrivateProfileStringW(L"resolver_rules",
                              (L"sub_" + idx + L"_rule_count").c_str(),
                              std::to_wstring((int)rules.size()).c_str(),
                              GetIniPath().c_str());
  WritePrivateProfileStringW(L"resolver_rules",
                              (L"sub_" + idx + L"_cache").c_str(),
                              base.c_str(), GetIniPath().c_str());
  Config::Instance().ReloadConfig();
  return true;
}

bool RefreshSubscription(int index, std::wstring& error) {
  const auto& subs = config.GetResolverSubscriptions();
  if (index < 0 || index >= (int)subs.size()) {
    error = L"订阅不存在";
    return false;
  }
  const auto& sub = subs[index];
  std::string text;
  if (!WinHttpGetString(WStringToUtf8(sub.url), text, error)) return false;

  std::vector<ResolvedRule> rules;
  int invalid = 0;
  ParseHostsText(text, rules, invalid);

  int max_total = config.GetResolverMaxTotal();
  int current = GetTotalRuleCount();
  int old = sub.rule_count;
  int prospective = current - old + (int)rules.size();
  if (sub.enabled && prospective > max_total) {
    error = L"刷新失败：规则总数将达 " + std::to_wstring(prospective) +
           L" 超出上限 " + std::to_wstring(max_total) + L"，已保留旧内容";
    return false;
  }

  if (!WriteCacheFile(sub.cache, rules)) {
    error = L"写入缓存失败";
    return false;
  }
  WriteSubInt(index, L"_rule_count", (long long)rules.size());
  WriteSubInt(index, L"_last_refresh", NowUnix());
  Config::Instance().ReloadConfig();
  return true;
}

bool RemoveSubscription(int index) {
  const auto& subs = config.GetResolverSubscriptions();
  if (index < 0 || index >= (int)subs.size()) return false;
  const auto& sub = subs[index];
  std::wstring idx = std::to_wstring(index + 1);
  const wchar_t* keys[] = {L"_name",  L"_url",  L"_enabled",
                           L"_last_refresh", L"_rule_count", L"_cache"};
  for (const wchar_t* k : keys)
    WritePrivateProfileStringW(L"resolver_rules", (L"sub_" + idx + k).c_str(),
                                nullptr, GetIniPath().c_str());
  DeleteFileW(CacheFilePath(sub.cache).c_str());
  Config::Instance().ReloadConfig();
  return true;
}

bool SetSubscriptionEnabled(int index, bool enabled) {
  const auto& subs = config.GetResolverSubscriptions();
  if (index < 0 || index >= (int)subs.size()) return false;
  WriteSubInt(index, L"_enabled", enabled ? 1 : 0);
  Config::Instance().ReloadConfig();
  return true;
}

void SetRefreshInterval(int hours) {
  if (hours < 0) hours = 0;
  WritePrivateProfileStringW(L"resolver_rules", L"refresh_interval",
                              std::to_wstring(hours).c_str(),
                              GetIniPath().c_str());
  Config::Instance().ReloadConfig();
}

bool ExportRules(const std::string& path_utf8, std::wstring& error) {
  std::vector<ResolvedRule> rules = GetEffectiveRules();
  if (rules.empty()) {
    error = L"没有生效规则可导出（请先启用订阅并刷新）";
    return false;
  }
  std::string text = RulesToHostsText(rules);
  std::wstring path;
  if (!path_utf8.empty()) {
    path = Utf8ToWstring(path_utf8);
  } else {
    std::wstring dir = GetAbsolutePath(
        CanonicalizePath(GetAppDir() + L"\\..\\Data\\resolver_export"));
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    time_t t = time(nullptr);
    struct tm tmval;
    localtime_s(&tmval, &t);
    wchar_t ts[64];
    swprintf_s(ts, 64, L"%04d%02d%02d_%02d%02d%02d", tmval.tm_year + 1900,
               tmval.tm_mon + 1, tmval.tm_mday, tmval.tm_hour, tmval.tm_min,
               tmval.tm_sec);
    path = dir + L"\\hosts." + ts + L".txt";
  }
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    error = L"创建导出文件失败";
    return false;
  }
  DWORD written = 0;
  WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
  CloseHandle(h);
  return true;
}

void StartResolverRefresh() {
  if (!config.IsResolverEnabled()) return;
  if (config.GetResolverRefreshInterval() <= 0) return;
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread([]() {
    while (true) {
      Sleep(3600 * 1000);
      Config::Instance().ReloadConfig();
      if (!config.IsResolverEnabled()) continue;
      int interval = config.GetResolverRefreshInterval();
      if (interval <= 0) continue;
      long long now = NowUnix();
      const auto& subs = config.GetResolverSubscriptions();
      for (size_t i = 0; i < subs.size(); ++i) {
        const auto& sub = subs[i];
        if (!sub.enabled) continue;
        if (sub.last_refresh > 0 &&
            (now - sub.last_refresh) < (long long)interval * 3600)
          continue;
        std::wstring err;
        RefreshSubscription((int)i, err);
      }
    }
  }).detach();
}

}  // namespace resolver
