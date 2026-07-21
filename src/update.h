#ifndef CHROME_GREEN_SRC_UPDATE_H_
#define CHROME_GREEN_SRC_UPDATE_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Chrome update channel
enum class UpdateChannel {
  kStable,
  kBeta,
  kDev,
  kCanary,
};

// CPU architecture for download
enum class UpdateArch {
  kX64,
  kX86,
  kARM64,
};

// Information about an available Chrome update
struct UpdateInfo {
  std::string version;           // e.g. "132.0.6834.59"
  std::vector<std::string> urls; // download URLs (multiple CDN mirrors)
  std::string sha256;            // uppercase hex SHA-256
  std::string sha1;              // uppercase hex SHA-1
  int64_t size = 0;              // file size in bytes
  int64_t timestamp = 0;         // when this info was obtained (Unix ms)
  bool has_update = false;       // whether a newer version exists
};

// State machine for the update lifecycle
enum class UpdateState {
  kIdle,          // no update activity
  kChecking,      // querying Omaha for version info
  kAvailable,     // newer version found, not yet downloading
  kDownloading,   // download in progress
  kReady,         // download complete, waiting for user to trigger apply+restart
  kApplying,      // extracting archive before restart
  kPendingApply,  // extracted to temp dir, files will be moved on next launch
  kError,         // last operation failed
};

// Persistent state written to chrome_green_update.json
struct UpdateStateData {
  UpdateState state = UpdateState::kIdle;
  std::string current_version;    // installed Chrome version
  std::string latest_version;     // latest available version
  UpdateChannel channel = UpdateChannel::kStable;
  UpdateArch arch = UpdateArch::kX64;
  int download_progress = 0;      // 0-100
  int64_t download_size = 0;      // total bytes to download
  int64_t downloaded_bytes = 0;   // bytes downloaded so far
  int64_t download_speed = 0;     // current download speed, bytes/sec (smoothed)
  int64_t download_eta = 0;       // seconds remaining; -1 = indeterminate/unknown
  std::string download_path;      // local path of downloaded .7z
  std::string sha256;             // expected hash
  std::string error_message;      // last error
  int64_t last_check_time = 0;    // Unix timestamp of last check
  bool auto_check = false;
  bool auto_download = true;
  std::string proxy;              // custom proxy, empty = system default
  std::string proxy_type;         // "HTTP", "SOCKS5", or "GH_PROXY" (GitHub release mirror)
  bool proxy_chrome_download = false;  // whether to use proxy for Chrome download
  int download_source = 1;        // 0=edgedl.me.gvt1.com, 1=dl.google.com, 2=www.google.com, 3=redirector.gvt1.com
  // Self-update (chrome_green version.dll)
  std::string self_latest_version;   // latest chrome_green release tag
  std::string self_download_url;     // download URL for version.dll
  std::string self_download_path;    // local path of downloaded DLL
  int64_t self_download_size = 0;
  int self_self_download_progress = 0;  // 0-100
  bool self_update_ready = false;       // pending replacement on next launch
  std::string self_release_notes;       // release notes
  bool self_has_update = false;         // backend-derived: remote release > local build
  bool self_downloading = false;        // self-update download/extract in progress
};

// Global state (thread-safe)
extern UpdateStateData g_update_state;
extern std::mutex g_update_mutex;

// Get state file path: <DLL dir>/../Data/chrome_green_update.json
// (Chrome's data directory; ".." = parent of the DLL dir)
std::wstring GetUpdateStatePath();

// Load / save state from/to JSON file
void LoadUpdateState();
void SaveUpdateState();

// Get a snapshot of the current state (thread-safe)
UpdateStateData GetUpdateStateSnapshot();

// Update state (thread-safe, also saves to file)
void SetUpdateState(UpdateState state);
void SetUpdateError(const std::string& msg);

// Omaha protocol: check for updates
// Returns UpdateInfo with has_update=true if a newer version is available.
UpdateInfo CheckForUpdates(UpdateChannel channel, UpdateArch arch,
                           const std::string& proxy,
                           const std::string& proxy_type);

// Get the Chrome appid for a given channel
// Stable/Dev/Beta: {8A69D345-D564-463C-AFF1-A69D9E530F96}
// Canary: {4EA16AC7-FD5A-47C3-875B-DBF4A2008C20}
const char* GetAppId(UpdateChannel channel);

// Get the "ap" parameter for channel + arch
// e.g. "x64-stable-statsdef_1", "arm64-beta-statsdef_1"
std::string GetApParameter(UpdateChannel channel, UpdateArch arch);

// Self-update: check GitHub Releases for new chrome_green version.
// Returns UpdateInfo with has_update=true if a newer version is available.
// proxy/proxy_type: empty means no proxy (does NOT use system proxy).
UpdateInfo CheckSelfUpdate(const std::string& proxy = "",
                          const std::string& proxy_type = "");

// Self-update: download the new release zip and extract version.dll.
// proxy/proxy_type: empty means no proxy.
bool DownloadSelfUpdate(const std::string& url,
                        const std::wstring& save_path,
                        const std::wstring& extract_dir,
                        const std::string& proxy = "",
                        const std::string& proxy_type = "");

// Save/load self-update state
void SaveSelfUpdateState();
void LoadSelfUpdateState();

// Get channel name as string
const wchar_t* ChannelToString(UpdateChannel channel);
const char* ChannelToStringA(UpdateChannel channel);

// Get arch name as string
const char* ArchToString(UpdateArch arch);

// Parse channel from string
UpdateChannel ParseChannel(const std::wstring& str);

// Get installed Chrome version from chrome.exe file version
std::string GetInstalledChromeVersion();

// --- Debug log ring buffer ---

// Max number of log lines kept in memory.
constexpr size_t kMaxLogLines = 200;

extern std::deque<std::string> g_debug_logs;
extern std::mutex g_debug_log_mutex;

// Append a timestamped log entry (thread-safe).
void AddDebugLog(const std::string& msg);

// Get a copy of all log lines (thread-safe), newest last.
std::vector<std::string> GetDebugLogs();

// Clear all log lines (thread-safe).
void ClearDebugLogs();

// Escape a string for embedding in JSON (quotes, backslashes, control chars).
std::string JsonEscape(const std::string& s);

// UTF-8 encoding/decoding for wide string paths stored in JSON.
std::string WstrToUtf8(const std::wstring& wstr);
std::wstring Utf8ToWstr(const std::string& str);

#endif  // CHROME_GREEN_SRC_UPDATE_H_
