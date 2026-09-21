#ifndef CHROME_GREEN_SRC_CONFIG_H_
#define CHROME_GREEN_SRC_CONFIG_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "update.h"

class Config {
 public:
  static Config& Instance();

  // general
  const std::wstring& GetCommandLine() const { return command_line_; }
  const std::wstring& GetLaunchOnStartup() const { return launch_on_startup_; }
  const std::wstring& GetLaunchOnExit() const { return launch_on_exit_; }
  const std::optional<std::wstring>& GetUserDataDir() const {
    return user_data_dir_;
  }
  const std::optional<std::wstring>& GetDiskCacheDir() const {
    return disk_cache_dir_;
  }
  // Raw (un-expanded) values as written in chrome_green.ini — used by the
  // config page so the user edits the literal string (e.g. "%app%\..\Data")
  // rather than the resolved absolute path.
  const std::wstring& GetUserDataDirRaw() const { return user_data_dir_raw_; }
  const std::wstring& GetDiskCacheDirRaw() const {
    return disk_cache_dir_raw_;
  }
  // ChromeGreen data directory (config [general] cg_data_dir). Raw = literal
  // string from ini; resolved = absolute path from LoadCgDataDir(); root =
  // resolved + "\ChromeGreenData" (the fixed subdir that holds all of our data).
  const std::wstring& GetCgDataDirRaw() const { return cg_data_dir_raw_; }
  const std::optional<std::wstring>& GetCgDataDir() const {
    return cg_data_dir_;
  }
  const std::optional<std::wstring>& GetCgDataRoot() const {
    return cg_data_root_;
  }
  const std::wstring& GetTranslateKey() const { return translate_key_; }
  const std::wstring& GetBossKey() const { return boss_key_; }
  // Action hotkeys (open new window / batch-open URL group)
  const std::wstring& GetOpenNewWindowHotkey() const { return open_new_window_; }
  const std::wstring& GetOpenUrlGroupHotkey() const { return open_url_group_; }
  const std::vector<std::wstring>& GetUrlGroup() const { return url_group_; }
  bool IsShowPassword() const { return show_password_; }
  // Debug-log gate: when false, AddDebugLog() in update.cc is a no-op and the
  // config page hides the Logs tab. Defaults to false (off).
  bool IsDebugLog() const { return debug_log_; }
  // When true, ChromeGreen injects --test-type to suppress the
  // "unsupported command-line flag" infobar (e.g. from --host-resolver-rules).
  // Defaults to false (off).
  bool IsSuppressCmdlineWarning() const {
    return suppress_cmdline_warning_;
  }
  // When true, the updater opens the config page after an in-app update.
  // Defaults to false (off).
  bool IsOpenConfigAfterUpdate() const {
    return open_config_after_update_;
  }
  // When true, fix the taskbar context menu by registering localized jump-list
  // Tasks (New window / Incognito / Recent) under our stable per-install AUMID.
  // Experimental and ON by default. When off, a portable Chrome pinned to the
  // taskbar by any method shows only the system "Unpin" item in its right-click
  // menu (no Tasks are registered under the AUMID).
  bool IsFixTaskbarMenu() const { return fix_taskbar_menu_; }
  const std::string& GetTheme() const { return theme_; }
  const std::string& GetLanguage() const { return language_; }
  bool IsWin32K() const { return win32k_; }
  bool IsIgnorePolicies() const { return ignore_policies_; }
  bool IsSuppressFalseUpgradeNotification() const {
    return suppress_false_upgrade_notification_;
  }

  // tabs (ported from chrome_plus tabbookmark)
  bool IsKeepLastTab() const { return keep_last_tab_; }
  bool IsDoubleClickClose() const { return double_click_close_; }
  bool IsRightClickClose() const { return right_click_close_; }
  bool IsWheelTab() const { return wheel_tab_; }
  bool IsWheelTabWhenPressRightButton() const {
    return wheel_tab_when_press_rbutton_;
  }
  bool IsHoverTab() const { return hover_tab_; }
  int GetHoverTabDelay() const { return hover_tab_delay_; }
  int GetOpenUrlNewTabMode() const { return open_url_new_tab_; }
  int GetBookmarkNewTabMode() const { return bookmark_new_tab_; }
  bool IsNewTabDisable() const { return new_tab_disable_; }
  const std::wstring& GetDisableTabName() const { return disable_tab_name_; }
  const std::vector<std::wstring>& GetDisableTabNames() const {
    return disable_tab_names_;
  }

  // keymapping
  using KeyMappingPair = std::pair<std::wstring, std::wstring>;
  const auto& GetKeyMappings() const { return key_mappings_; }

  // resolver_rules (域名映射 / Chromium --host-resolver-rules)
  bool IsResolverEnabled() const { return resolver_enabled_; }
  int GetResolverRefreshInterval() const { return resolver_refresh_interval_; }
  int GetResolverMaxTotal() const { return resolver_max_total_; }
  struct ResolverSubscription {
    std::wstring name;
    std::wstring url;
    bool enabled = false;
    long long last_refresh = 0;  // unix time, 0 = never
    int rule_count = 0;
    std::wstring cache;  // cache file base name (e.g. sub_1)
  };
  const std::vector<ResolverSubscription>& GetResolverSubscriptions() const {
    return resolver_subs_;
  }

  // update
  UpdateChannel GetUpdateChannel() const { return update_channel_; }
  UpdateArch GetUpdateArch() const { return update_arch_; }
  bool IsAutoCheck() const { return auto_check_; }
  int GetCheckInterval() const { return check_interval_; }
  bool IsAutoDownload() const { return auto_download_; }
  bool KeepInstaller() const { return keep_installer_; }
  bool KeepOldVersions() const { return keep_old_versions_; }
  const std::string& GetUpdateProxy() const { return update_proxy_; }
  const std::string& GetUpdateProxyType() const { return update_proxy_type_; }
  bool ProxyChromeDownload() const { return proxy_chrome_download_; }
  int GetDownloadSource() const { return download_source_; }
  // Architecture is auto-detected from the system, not user-configurable
  static UpdateArch DetectArch();

  // Re-read chrome_green.ini so getters reflect values just written by
  // POST /api/config. The singleton is loaded once at startup; settings
  // saved at runtime would otherwise be invisible to GetConfigJson().
  void ReloadConfig() { LoadConfig(); }

  // If chrome_green.ini does not exist, create it with all default keys so the
  // file is present (and editable) on first launch. Called from LoadConfig().
  void EnsureIniExists();

  // --- ChromeGreen data directory migration ---
  // After the user changes cg_data_dir at runtime, move the old
  // <cg_data_dir>\ChromeGreenData (favicons + update state) to the new
  // location. Idempotent: no-op if old root is empty/equal/missing.
  void MigrateCgDataOnDirChange(const std::optional<std::wstring>& old_root,
                                const std::optional<std::wstring>& new_root);
  // Upgrade migration: move the legacy <APP>\favicons and
  // <DLL>\..\Data\chrome_green_update.json into the current ChromeGreenData.
  // Idempotent: skipped when the legacy paths no longer exist.
  void MigrateLegacyCgData();

 private:
  Config();
  ~Config() = default;
  Config(const Config&) = delete;
  Config& operator=(const Config&) = delete;

  void LoadConfig();
  void LoadKeyMappings();
  int LoadHoverTabDelay();
  int LoadOpenUrlNewTabMode();
  int LoadBookmarkNewTabMode();

  std::optional<std::wstring> LoadDirPath(const std::wstring& dir_type);
  std::optional<std::wstring> LoadCgDataDir();
  // Recursively copy src -> dst then delete src (cross-volume safe, since
  // MoveFileExW with MOVEFILE_COPY_ALLOWED does not delete the source).
  static bool CopyDirRecursive(const std::wstring& src,
                               const std::wstring& dst);

 private:
  // general
  std::wstring command_line_;
  std::wstring launch_on_startup_;
  std::wstring launch_on_exit_;
  std::optional<std::wstring> user_data_dir_;
  std::optional<std::wstring> disk_cache_dir_;
  std::wstring user_data_dir_raw_;
  std::wstring disk_cache_dir_raw_;
  std::optional<std::wstring> cg_data_dir_;
  std::wstring cg_data_dir_raw_;
  std::optional<std::wstring> cg_data_root_;
  std::wstring translate_key_;
  std::wstring boss_key_;
  // Action hotkeys (open new window / batch-open URL group)
  std::wstring open_new_window_;
  std::wstring open_url_group_;
  std::vector<std::wstring> url_group_;
  bool show_password_ = false;
  bool debug_log_ = false;
  bool suppress_cmdline_warning_ = false;
  // When true, the updater opens the config page (http://127.0.0.1:<port>) in
  // a new tab after an in-app update. Defaults to false (off).
  bool open_config_after_update_ = false;
  // Fix taskbar context menu (experimental). ON by default.
  bool fix_taskbar_menu_ = true;
  std::string theme_;
  std::string language_;
  bool win32k_;
  bool ignore_policies_;
  bool suppress_false_upgrade_notification_;

  // tabs (ported from chrome_plus tabbookmark)
  bool keep_last_tab_ = false;
  bool double_click_close_ = false;
  bool right_click_close_ = false;
  bool wheel_tab_ = false;
  bool wheel_tab_when_press_rbutton_ = false;
  bool hover_tab_ = false;
  int hover_tab_delay_ = 0;
  int open_url_new_tab_ = 0;
  int bookmark_new_tab_ = 0;
  bool new_tab_disable_ = false;
  std::wstring disable_tab_name_;
  std::vector<std::wstring> disable_tab_names_;

  // keymapping
  std::vector<KeyMappingPair> key_mappings_;

  // resolver_rules (域名映射)
  bool resolver_enabled_ = false;
  int resolver_refresh_interval_ = 0;  // 0 = manual
  int resolver_max_total_ = 800;
  std::vector<ResolverSubscription> resolver_subs_;

  // update
  UpdateChannel update_channel_ = UpdateChannel::kStable;
  UpdateArch update_arch_ = UpdateArch::kX64;
  bool auto_check_ = false;
  int check_interval_ = 24;
  bool auto_download_ = false;
  bool keep_installer_ = false;
  bool keep_old_versions_ = false;
  std::string update_proxy_;
  std::string update_proxy_type_ = "HTTP";
  bool proxy_chrome_download_ = false;
  int download_source_ = 1;  // 0=edgedl, 1=dl.google.com (default), 2=www.google.com
};

extern const Config& config;

// Global gate for in-memory debug logging (updated from config on LoadConfig).
extern bool g_enable_debug_log;

#endif  // CHROME_GREEN_SRC_CONFIG_H_
