#include "downloader.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "update.h"
#include "updater.h"
#include "utils.h"
#include "config.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shlwapi.lib")

Downloader& Downloader::Instance() {
  static Downloader instance;
  return instance;
}

Downloader::~Downloader() {
  Cancel();
  std::lock_guard<std::mutex> lock(join_mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}

// Delete the (partial) installer file, retrying briefly. Right after the
// download thread closes its handle, an antivirus scan or the indexer can
// still hold the file open for a moment — the old single-attempt
// DeleteFileW silently failed in that window and left broken partial
// installers behind in updates/.
static void DeleteInstallerFileWithRetry(const std::wstring& path) {
  if (path.empty()) return;
  for (int i = 0; i < 5; i++) {
    if (i > 0) Sleep(300);
    if (!PathFileExistsW(path.c_str())) return;  // already gone
    if (DeleteFileW(path.c_str())) return;
  }
  AddDebugLog("Could not delete partial installer: " + WstrToUtf8(path) +
              " (error=" + std::to_string(GetLastError()) + ")");
}

// Parse a URL into host and path components for WinHTTP.
struct UrlParts {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
  bool https = true;
};

static bool ParseUrl(const std::string& url, UrlParts& out) {
  // Expected format: https://host/path... or http://host/path...
  std::wstring url_w(url.begin(), url.end());

  if (url_w.starts_with(L"https://")) {
    out.https = true;
    out.port = INTERNET_DEFAULT_HTTPS_PORT;
    url_w = url_w.substr(8);
  } else if (url_w.starts_with(L"http://")) {
    out.https = false;
    out.port = INTERNET_DEFAULT_HTTP_PORT;
    url_w = url_w.substr(7);
  } else {
    return false;
  }

  auto slash_pos = url_w.find(L'/');
  if (slash_pos == std::wstring::npos) {
    out.host = url_w;
    out.path = L"/";
  } else {
    out.host = url_w.substr(0, slash_pos);
    out.path = url_w.substr(slash_pos);
  }

  // Handle port in host (host:port)
  auto colon_pos = out.host.find(L':');
  if (colon_pos != std::wstring::npos) {
    out.port = (INTERNET_PORT)std::stoul(out.host.substr(colon_pos + 1));
    out.host = out.host.substr(0, colon_pos);
  }

  return true;
}

// Build the WinHTTP proxy string from the user-configured proxy value and its
// type. For HTTP proxies we keep the value as-is (a plain "host:port" applies
// to all schemes in WinHTTP); any "scheme://" prefix the user may have typed
// is stripped. For SOCKS5 we ensure the "socks=" prefix WinHTTP expects.
// We deliberately do NOT consult the system/IE proxy or PAC: the download host
// is reachable directly, and a proxy is only used when the user explicitly
// enabled it AND configured one.
static std::wstring BuildProxyString(const std::string& proxy,
                                     const std::string& type) {
  std::wstring p(proxy.begin(), proxy.end());
  if (type == "SOCKS5") {
    if (p.find(L"socks") == std::wstring::npos) {
      p = L"socks=" + p;
    }
    return p;
  }
  // HTTP: strip a scheme prefix if the user included one.
  size_t pos = p.find(L"://");
  if (pos != std::wstring::npos) {
    p = p.substr(pos + 3);
  }
  return p;
}

// Map a WinHTTP error code to a short, human-readable reason.
static std::string WinHttpErrorText(DWORD err) {
  switch (err) {
    case ERROR_WINHTTP_TIMEOUT: return "timeout";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "name not resolved";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection error";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS/SSL failure";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
    default: return "code " + std::to_string(err);
  }
}

bool VerifyFileSHA256(const std::wstring& path, const std::string& expected_hash) {
  // Use BCrypt to compute SHA-256
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  DWORD hash_length = 0;
  DWORD data_len = 0;

  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
    return false;
  }

  if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                        reinterpret_cast<PUCHAR>(&hash_length),
                        sizeof(hash_length), &data_len, 0) != 0) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return false;
  }

  if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return false;
  }

  // Read file in chunks and feed to hash
  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return false;
  }

  constexpr DWORD kBufSize = 65536;
  std::vector<BYTE> buffer(kBufSize);
  DWORD bytes_read = 0;
  bool ok = true;

  while (ReadFile(hFile, buffer.data(), kBufSize, &bytes_read, nullptr) && bytes_read > 0) {
    if (BCryptHashData(hHash, buffer.data(), bytes_read, 0) != 0) {
      ok = false;
      break;
    }
  }

  CloseHandle(hFile);

  if (!ok) {
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return false;
  }

  std::vector<BYTE> hash_bytes(hash_length);
  if (BCryptFinishHash(hHash, hash_bytes.data(), hash_length, 0) != 0) {
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return false;
  }

  BCryptDestroyHash(hHash);
  BCryptCloseAlgorithmProvider(hAlg, 0);

  // Convert to uppercase hex
  static const char hex[] = "0123456789ABCDEF";
  std::string actual;
  actual.reserve(hash_length * 2);
  for (BYTE b : hash_bytes) {
    actual += hex[b >> 4];
    actual += hex[b & 0xF];
  }

  // Case-insensitive comparison
  return _stricmp(actual.c_str(), expected_hash.c_str()) == 0;
}

void Downloader::DownloadThread(std::string url, std::wstring path,
                                 std::string expected_sha256) {
  downloading_.store(true);
  cancel_requested_.store(false);
  reset_requested_.store(false);

  AddDebugLog("DownloadThread started url=" + url);

  SetUpdateState(UpdateState::kDownloading);

  // Ensure the target directory exists
  std::wstring dir = path.substr(0, path.find_last_of(L"\\"));
  CreateDirectoryW(dir.c_str(), nullptr);

  // Proxy policy: the download host is reachable directly, so direct is the
  // default. We only route through a proxy when BOTH the "download via proxy"
  // toggle is enabled AND a download proxy is configured. No system/PAC
  // fallback — the user controls proxy usage explicitly.
  std::wstring proxy_server;
  bool use_proxy = false;
  {
    auto state = GetUpdateStateSnapshot();
    // GH_PROXY only accelerates GitHub *release* downloads (handled in
    // update.cc DownloadSelfUpdate). For Chrome installer downloads it must NOT
    // be used even when proxy_chrome_download is on, so force direct here.
    if (state.proxy_chrome_download && !state.proxy.empty() &&
        state.proxy_type != "GH_PROXY") {
      proxy_server = BuildProxyString(state.proxy, state.proxy_type);
      use_proxy = true;
      AddDebugLog("Download using configured proxy: " + state.proxy);
    } else {
      if (state.proxy_type == "GH_PROXY") {
        AddDebugLog("Download using direct connection (GH_PROXY only proxies GitHub release downloads)");
      } else {
        AddDebugLog("Download using direct connection");
      }
    }
  }

  // --- Always download over HTTPS ---
  // Omaha hands us an http:// dl.google.com URL, but on some networks a
  // transparent middlebox empties plaintext http responses (HTTP 204 with no
  // body), while the identical https:// request succeeds. We therefore always
  // upgrade to https and never attempt plaintext http.
  std::string download_url = url;
  if (download_url.starts_with("http://")) {
    download_url = "https://" + download_url.substr(7);
  }

  struct Conn { HINTERNET session = nullptr, connect = nullptr, request = nullptr; };
  auto CloseConn = [](Conn& c) {
    if (c.request) WinHttpCloseHandle(c.request);
    if (c.connect) WinHttpCloseHandle(c.connect);
    if (c.session) WinHttpCloseHandle(c.session);
    c.session = nullptr; c.connect = nullptr; c.request = nullptr;
  };

  // Open a GET request for `u`. When range_from > 0, send a "Range: bytes=N-"
  // header so an interrupted download resumes from the bytes already on disk.
  // Returns true only for HTTP 200 (full body) or 206 (partial/resumed body);
  // *sc_out receives the status code.
  auto OpenRequest = [&](const std::string& u, int64_t range_from,
                         Conn& conn_out, DWORD* sc_out) -> bool {
    UrlParts parts;
    if (!ParseUrl(u, parts)) {
      AddDebugLog("Download: invalid URL " + u);
      return false;
    }
    HINTERNET hSession = WinHttpOpen(L"ChromeGreen Updater",
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { AddDebugLog("Download: WinHttpOpen failed"); return false; }
    if (use_proxy && !proxy_server.empty()) {
      WINHTTP_PROXY_INFO pi;
      pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
      pi.lpszProxy = const_cast<LPWSTR>(proxy_server.c_str());
      pi.lpszProxyBypass = WINHTTP_NO_PROXY_BYPASS;
      WinHttpSetOption(hSession, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
    }
    WinHttpSetTimeouts(hSession, 30000, 30000, 60000, 60000);
    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(),
                                        parts.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
      WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }
    if (range_from > 0) {
      std::wstring range_header =
          L"Range: bytes=" + std::to_wstring(range_from) + L"-";
      WinHttpAddRequestHeaders(hRequest, range_header.c_str(), (ULONG)-1,
                               WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
      DWORD err = GetLastError();
      AddDebugLog("Download: WinHttpSendRequest failed (" + WinHttpErrorText(err) + ")");
      WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
      return false;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
      DWORD err = GetLastError();
      AddDebugLog("Download: WinHttpReceiveResponse failed (" + WinHttpErrorText(err) + ")");
      WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
      return false;
    }
    DWORD sc = 0, sc_size = sizeof(sc);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sc_size, WINHTTP_NO_HEADER_INDEX);
    if (sc != 200 && sc != 206) {
      // Capture the Location header so a non-200 is debuggable instead of a
      // bare "HTTP 204" — a redirect target or interception notice shows up.
      wchar_t loc[2048] = {0}; DWORD loc_sz = sizeof(loc);
      std::string loc_u;
      if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                              WINHTTP_HEADER_NAME_BY_INDEX, loc, &loc_sz,
                              WINHTTP_NO_HEADER_INDEX)) {
        loc_u = WstrToUtf8(std::wstring(loc));
      }
      AddDebugLog("Download attempt HTTP " + std::to_string(sc) +
                  " url=" + u + (loc_u.empty() ? "" : " location=" + loc_u));
      WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
      return false;
    }
    conn_out.session = hSession; conn_out.connect = hConnect; conn_out.request = hRequest;
    *sc_out = sc;
    return true;
  };

  AddDebugLog("Download attempt: " + download_url);

  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.download_size = 0;
    g_update_state.downloaded_bytes = 0;
    g_update_state.download_progress = 0;
    g_update_state.download_speed = 0;
    g_update_state.download_eta = -1;  // unknown until we have samples
  }

  constexpr DWORD kChunkSize = 65536;
  std::vector<BYTE> buffer(kChunkSize);
  HANDLE hFile = INVALID_HANDLE_VALUE;
  int64_t total_downloaded = 0;
  int64_t total_size = 0;
  bool success = false;
  std::string error_msg;  // set at each give-up point, shown in the UI

  // Download in chunks. Use WinHttpReadData directly (blocking) instead of
  // polling WinHttpQueryDataAvailable, which can return 0 before the server
  // has sent any body bytes and be mistaken for EOF on slow connections.
  //
  // Long downloads (the Chrome installer is 100+ MB) can hit a mid-transfer
  // stall longer than the 60s WinHTTP receive timeout, or the connection can
  // be reset by the CDN / a flaky Wi-Fi. That used to abort the whole
  // download with "Download failed". Instead, reconnect with a Range header
  // and resume from the bytes already on disk; only give up after several
  // failed reconnects.
  const int kMaxAttempts = 6;  // 1 initial try + 5 resumes
  int attempt = 0;

  while (!cancel_requested_.load()) {
    bool resuming = total_downloaded > 0;
    Conn conn;
    DWORD sc = 0;
    if (!OpenRequest(download_url, resuming ? total_downloaded : 0, conn, &sc)) {
      if (++attempt >= kMaxAttempts) {
        AddDebugLog("Download FAILED: " + download_url +
                    " (original url=" + url + ")");
        error_msg = "HTTP error (download blocked; try a proxy/VPN)";
        break;
      }
      DWORD delay = (DWORD)(attempt * 1000);
      if (delay > 5000) delay = 5000;
      AddDebugLog("Download: connect failed, retry " + std::to_string(attempt) +
                  "/" + std::to_string(kMaxAttempts - 1) + " in " +
                  std::to_string(delay) + "ms");
      Sleep(delay);
      continue;
    }

    // 206 while resuming → the server honored the Range; keep the bytes on
    // disk and append. 200 while resuming → the server ignored the Range
    // (or a transparent proxy answered); restart from scratch.
    if (resuming && sc == 200) {
      AddDebugLog("Download: server ignored Range (HTTP 200), restarting from 0");
      total_downloaded = 0;
      resuming = false;
    }
    bool append = resuming && sc == 206;

    // Content-Length: full size for 200, remaining bytes for 206.
    wchar_t content_length_buf[32] = {0};
    DWORD cl_size = sizeof(content_length_buf);
    WinHttpQueryHeaders(conn.request, WINHTTP_QUERY_CONTENT_LENGTH,
                        WINHTTP_HEADER_NAME_BY_INDEX, content_length_buf, &cl_size,
                        WINHTTP_NO_HEADER_INDEX);
    int64_t content_length = (cl_size > 0) ? _wtoi64(content_length_buf) : 0;
    total_size = append ? (total_downloaded + content_length) : content_length;

    AddDebugLog(std::string("Download ") +
                (append ? "resumed at " : "started: ") +
                (append ? std::to_string(total_downloaded) + " of " + std::to_string(total_size) + " bytes"
                        : url + " (size=" + std::to_string(total_size) + ")"));

    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.download_size = total_size;
      g_update_state.downloaded_bytes = total_downloaded;
      g_update_state.download_progress = total_size > 0
          ? (int)((total_downloaded * 100) / total_size) : 0;
    }

    // (Re)open the output file: truncate for a fresh start, append when
    // resuming (also trimming anything past the resume point).
    if (hFile != INVALID_HANDLE_VALUE) {
      CloseHandle(hFile);
      hFile = INVALID_HANDLE_VALUE;
    }
    hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                        append ? OPEN_ALWAYS : CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
      CloseConn(conn);
      AddDebugLog("Download: cannot create output file");
      error_msg = "Cannot create output file";
      break;
    }
    if (append) {
      LARGE_INTEGER pos;
      pos.QuadPart = total_downloaded;
      if (!SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN)) {
        AddDebugLog("Download: seek failed, restarting from 0");
        CloseConn(conn);
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        total_downloaded = 0;
        if (++attempt >= kMaxAttempts) {
          error_msg = "Download failed";
          break;
        }
        Sleep(1000);
        continue;
      }
      SetEndOfFile(hFile);  // trim any garbage past the resume point
    }

    // --- Chunked read loop for this connection ---
    int last_logged_progress = -1;
    bool read_ok = true;

    // Speed / ETA sampling state. WinHttpReadData can block for a while and
    // then deliver a burst of bytes, so a per-chunk speed would be noisy. We
    // sample on a time cadence and smooth with an exponential moving average
    // instead. Reset per connection so retry delays don't skew the EMA.
    auto last_sample_time = std::chrono::steady_clock::now();
    int64_t last_sample_bytes = total_downloaded;
    double smooth_speed = 0.0;  // bytes/sec, EMA-smoothed

    while (!cancel_requested_.load()) {
      DWORD bytes_read = 0;
      if (!WinHttpReadData(conn.request, buffer.data(), kChunkSize, &bytes_read)) {
        AddDebugLog("Download read error: " + WinHttpErrorText(GetLastError()) +
                    " after " + std::to_string(total_downloaded) + " bytes");
        read_ok = false;
        break;
      }
      if (bytes_read == 0) {
        // Normal end of stream
        break;
      }

      DWORD bytes_written = 0;
      if (!WriteFile(hFile, buffer.data(), bytes_read, &bytes_written, nullptr) ||
          bytes_written != bytes_read) {
        AddDebugLog("Download write error: " + WinHttpErrorText(GetLastError()));
        read_ok = false;
        break;
      }

      total_downloaded += bytes_read;

      int progress = total_size > 0
          ? (int)((total_downloaded * 100) / total_size) : 0;

      // --- Speed / ETA sampling ---
      // Sample on a ~0.3s time cadence (not per chunk) and smooth with an EMA so
      // the displayed speed/ETA is stable instead of spiking with each read burst.
      auto now_ts = std::chrono::steady_clock::now();
      double dt = std::chrono::duration<double>(now_ts - last_sample_time).count();
      if (dt >= 0.3 || (total_size > 0 && total_downloaded >= total_size)) {
        int64_t delta = total_downloaded - last_sample_bytes;
        double inst = (dt > 0.0) ? (delta / dt) : 0.0;
        if (smooth_speed <= 0.0) {
          smooth_speed = inst;
        } else {
          smooth_speed = smooth_speed * 0.6 + inst * 0.4;
        }
        last_sample_bytes = total_downloaded;
        last_sample_time = now_ts;
        int64_t eta = -1;
        if (total_size > 0 && smooth_speed > 1024.0) {
          eta = (int64_t)((total_size - total_downloaded) / smooth_speed);
          if (eta < 0) eta = 0;
        }
        {
          std::lock_guard<std::mutex> lock(g_update_mutex);
          g_update_state.downloaded_bytes = total_downloaded;
          g_update_state.download_progress = progress;
          g_update_state.download_speed = (int64_t)smooth_speed;
          g_update_state.download_eta = eta;
        }
      } else {
        // Keep progress/bytes current every chunk even between samples.
        std::lock_guard<std::mutex> lock(g_update_mutex);
        g_update_state.downloaded_bytes = total_downloaded;
        g_update_state.download_progress = progress;
      }

      if (progress > 0 && progress != last_logged_progress &&
          (progress % 10 == 0 || progress == 100)) {
        last_logged_progress = progress;
        AddDebugLog("Download progress: " + std::to_string(progress) + "%");
      }
    }

    CloseConn(conn);
    CloseHandle(hFile);
    hFile = INVALID_HANDLE_VALUE;

    if (cancel_requested_.load()) break;
    if (read_ok) {
      success = true;
      break;
    }

    // Transfer broke mid-stream — reconnect and resume from the bytes we
    // already have on disk.
    if (++attempt >= kMaxAttempts) {
      AddDebugLog("Download: giving up after " + std::to_string(kMaxAttempts) +
                  " attempts (" + std::to_string(total_downloaded) + " bytes)");
      error_msg = "Download failed (network interrupted)";
      break;
    }
    DWORD delay = (DWORD)(attempt * 1000);
    if (delay > 5000) delay = 5000;
    AddDebugLog("Download: resuming at " + std::to_string(total_downloaded) +
                " bytes (retry " + std::to_string(attempt) + "/" +
                std::to_string(kMaxAttempts - 1) + " in " +
                std::to_string(delay) + "ms)");
    Sleep(delay);
  }

  if (hFile != INVALID_HANDLE_VALUE) {
    CloseHandle(hFile);
  }

  if (cancel_requested_.load()) {
    DeleteInstallerFileWithRetry(path);
    // The download wrote to <install>\updates\<file>.7z. After the partial file
    // is removed, the updates/ directory is now empty — clean it up so a
    // cancelled download doesn't leave a stray empty folder behind. Only when
    // the installer is not kept (consistent with the startup cleanup): when
    // keep_installer is ON the folder is the user's drop zone for offline
    // packages and must stay. RemoveDirectoryW only succeeds on an empty dir,
    // so anything still present (a retained installer, the GUI updater exe)
    // keeps the folder safely in place.
    if (!config.KeepInstaller()) {
      std::wstring updates_dir = GetSelfDllDir() + L"\\updates";
      RemoveDirectoryW(updates_dir.c_str());
    }
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.download_speed = 0;
      g_update_state.download_eta = 0;
    }
    // A reset already moved the state to kIdle — don't resurrect
    // "available" on top of it.
    if (!reset_requested_.load()) {
      SetUpdateState(UpdateState::kAvailable);
    }
    downloading_.store(false);
    return;
  }

  if (!success) {
    DeleteInstallerFileWithRetry(path);
    {
      std::lock_guard<std::mutex> lock(g_update_mutex);
      g_update_state.download_speed = 0;
      g_update_state.download_eta = 0;
    }
    if (!reset_requested_.load()) {
      SetUpdateError(error_msg.empty() ? "Download failed" : error_msg);
    }
    downloading_.store(false);
    return;
  }

  // Verify SHA-256
  if (!expected_sha256.empty() && !VerifyFileSHA256(path, expected_sha256)) {
    DeleteInstallerFileWithRetry(path);
    if (!reset_requested_.load()) {
      SetUpdateError("SHA-256 verification failed");
    }
    downloading_.store(false);
    return;
  }

  // Download complete
  {
    std::lock_guard<std::mutex> lock(g_update_mutex);
    g_update_state.download_progress = 100;
    g_update_state.download_speed = 0;
    g_update_state.download_eta = 0;
    g_update_state.download_path = WstrToUtf8(path);
  }

  SetUpdateState(UpdateState::kReady);
  SaveUpdateState();

  // Extract immediately — no need to wait for the user to click "Restart".
  // This saves a step and shows "pending apply" status to indicate extraction
  // is in progress before the restart.
  ApplyPendingUpdate();

  downloading_.store(false);
}

bool Downloader::Start(const std::string& url, const std::wstring& path,
                       const std::string& expected_sha256) {
  std::lock_guard<std::mutex> lock(join_mutex_);
  if (downloading_.load()) return false;

  if (thread_.joinable()) {
    thread_.join();
  }

  thread_ = std::thread(&Downloader::DownloadThread, this, url, path, expected_sha256);
  return true;
}

void Downloader::Cancel() {
  cancel_requested_.store(true);
}

void Downloader::CancelForReset() {
  reset_requested_.store(true);
  cancel_requested_.store(true);
}

void Downloader::Wait() {
  std::lock_guard<std::mutex> lock(join_mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}
