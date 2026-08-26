#ifndef CHROME_GREEN_SRC_DOWNLOADER_H_
#define CHROME_GREEN_SRC_DOWNLOADER_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Background download manager for Chrome update packages.
// Downloads from a URL to a local file, tracking progress in g_update_state.
class Downloader {
 public:
  static Downloader& Instance();

  // Start downloading from `url` to `path`.
  // Expected SHA-256 (uppercase hex) for verification.
  // Returns false if a download is already in progress.
  bool Start(const std::string& url, const std::wstring& path,
             const std::string& expected_sha256);

  // Cancel the current download (async, the thread will exit).
  void Cancel();

  // Cancel the current download AND suppress its exit-path state transition.
  // Used by /api/reset: the handler sets the state to kIdle itself, and the
  // download thread must not overwrite it with kAvailable/kError afterwards.
  void CancelForReset();

  // Is a download currently in progress?
  bool IsDownloading() const { return downloading_.load(); }

  // Wait for the download thread to finish.
  void Wait();

 private:
  Downloader() = default;
  ~Downloader();

  void DownloadThread(std::string url, std::wstring path,
                      std::string expected_sha256);

  std::thread thread_;
  std::atomic<bool> downloading_{false};
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> reset_requested_{false};
  // Serializes join/replace operations on thread_ so a background reset
  // thread (Wait) and a new Start call never join the same std::thread
  // concurrently (which is undefined behavior).
  std::mutex join_mutex_;
};

// Verify a file's SHA-256 hash (uppercase hex).
// Returns true if the hash matches.
bool VerifyFileSHA256(const std::wstring& path, const std::string& expected_hash);

#endif  // CHROME_GREEN_SRC_DOWNLOADER_H_
