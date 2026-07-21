#ifndef CHROME_GREEN_SRC_DOWNLOADER_H_
#define CHROME_GREEN_SRC_DOWNLOADER_H_

#include <atomic>
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
};

// Verify a file's SHA-256 hash (uppercase hex).
// Returns true if the hash matches.
bool VerifyFileSHA256(const std::wstring& path, const std::string& expected_hash);

#endif  // CHROME_GREEN_SRC_DOWNLOADER_H_
