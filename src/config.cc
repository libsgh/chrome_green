#include "config.h"

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "utils.h"

// Defined in update.cc; AddDebugLog() no-ops while this is false.
extern bool g_enable_debug_log;

Config& Config::Instance() {
  static Config instance;
  return instance;
}

Config::Config() {
  LoadConfig();
}

void Config::LoadConfig() {
  // If the INI is missing, materialize a full default file first so it exists
  // on disk (and is editable) after the first launch.
  EnsureIniExists();

  // general
  command_line_ = GetIniString(L"general", L"command_line", L"");
  launch_on_startup_ = GetIniString(L"general", L"launch_on_startup", L"");
  launch_on_exit_ = GetIniString(L"general", L"launch_on_exit", L"");
  user_data_dir_raw_ = GetIniString(L"general", L"data_dir", L"%app%\\..\\Data");
  disk_cache_dir_raw_ = GetIniString(L"general", L"cache_dir", L"%app%\\..\\Cache");
  user_data_dir_ = LoadDirPath(L"data");
  disk_cache_dir_ = LoadDirPath(L"cache");
  translate_key_ = GetIniString(L"general", L"translate_key", L"");
  boss_key_ = GetIniString(L"general", L"boss_key", L"");
  open_new_window_ = GetIniString(L"general", L"open_new_window", L"");
  open_url_group_ = GetIniString(L"general", L"open_url_group", L"");
  // URL group: [url_group] section, numbered keys 1,2,3,... (capped to avoid
  // an infinite loop if the section is malformed).
  url_group_.clear();
  for (int i = 1; i <= 256; ++i) {
    std::wstring value = GetIniString(L"url_group", std::to_wstring(i), L"");
    if (value.empty()) {
      break;
    }
    url_group_.push_back(std::move(value));
  }
  std::wstring theme_w = GetIniString(L"general", L"theme", L"auto");
  theme_ = std::string(theme_w.begin(), theme_w.end());
  std::wstring lang_w = GetIniString(L"general", L"language", L"auto");
  language_ = std::string(lang_w.begin(), lang_w.end());
  win32k_ = ::GetPrivateProfileIntW(L"general", L"win32k", 0,
                                    GetIniPath().c_str()) != 0;
  ignore_policies_ = ::GetPrivateProfileIntW(L"general", L"ignore_policies", 0,
                                             GetIniPath().c_str()) != 0;
  suppress_false_upgrade_notification_ =
      ::GetPrivateProfileIntW(L"general",
                              L"suppress_false_upgrade_notification", 0,
                              GetIniPath().c_str()) != 0;
  show_password_ = ::GetPrivateProfileIntW(L"general", L"show_password", 1,
                                           GetIniPath().c_str()) != 0;
  debug_log_ = ::GetPrivateProfileIntW(L"general", L"debug_log", 0,
                                       GetIniPath().c_str()) != 0;
  suppress_cmdline_warning_ =
      ::GetPrivateProfileIntW(L"general", L"suppress_cmdline_warning", 0,
                              GetIniPath().c_str()) != 0;
  open_config_after_update_ =
      ::GetPrivateProfileIntW(L"general", L"open_config_after_update", 0,
                              GetIniPath().c_str()) != 0;

  // tabs (ported from chrome_plus tabbookmark)
  keep_last_tab_ = ::GetPrivateProfileIntW(L"tabs", L"keep_last_tab", 1,
                                           GetIniPath().c_str()) != 0;
  double_click_close_ = ::GetPrivateProfileIntW(L"tabs", L"double_click_close",
                                                1, GetIniPath().c_str()) != 0;
  right_click_close_ = ::GetPrivateProfileIntW(L"tabs", L"right_click_close", 0,
                                               GetIniPath().c_str()) != 0;
  wheel_tab_ = ::GetPrivateProfileIntW(L"tabs", L"wheel_tab", 1,
                                       GetIniPath().c_str()) != 0;
  wheel_tab_when_press_rbutton_ =
      ::GetPrivateProfileIntW(L"tabs", L"wheel_tab_when_press_rbutton", 1,
                              GetIniPath().c_str()) != 0;
  hover_tab_ = ::GetPrivateProfileIntW(L"tabs", L"hover_tab", 0,
                                       GetIniPath().c_str()) != 0;
  hover_tab_delay_ = LoadHoverTabDelay();
  open_url_new_tab_ = LoadOpenUrlNewTabMode();
  bookmark_new_tab_ = LoadBookmarkNewTabMode();
  new_tab_disable_ = ::GetPrivateProfileIntW(L"tabs", L"new_tab_disable", 1,
                                             GetIniPath().c_str()) != 0;
  disable_tab_name_ = GetIniString(L"tabs", L"new_tab_disable_name", L"");
  disable_tab_names_ = StringSplit(disable_tab_name_, L',', L"\"");

  // keymapping
  LoadKeyMappings();

  // update
  update_channel_ = ParseChannel(
      GetIniString(L"update", L"channel", L"stable"));
  // Architecture is auto-detected from the running process (see DetectArch)
  update_arch_ = DetectArch();
  auto_check_ = ::GetPrivateProfileIntW(L"update", L"auto_check", 0,
                                         GetIniPath().c_str()) != 0;
  check_interval_ = ::GetPrivateProfileIntW(L"update", L"check_interval", 24,
                                             GetIniPath().c_str());
  auto_download_ = ::GetPrivateProfileIntW(L"update", L"auto_download", 0,
                                            GetIniPath().c_str()) != 0;
  keep_installer_ = ::GetPrivateProfileIntW(L"update", L"keep_installer", 0,
                                             GetIniPath().c_str()) != 0;
  keep_old_versions_ = ::GetPrivateProfileIntW(L"update", L"keep_old_versions", 0,
                                                GetIniPath().c_str()) != 0;
  std::wstring proxy_w = GetIniString(L"update", L"proxy", L"");
  update_proxy_ = std::string(proxy_w.begin(), proxy_w.end());
  std::wstring proxy_type_w = GetIniString(L"update", L"proxy_type", L"HTTP");
  update_proxy_type_ = std::string(proxy_type_w.begin(), proxy_type_w.end());
  proxy_chrome_download_ =
      ::GetPrivateProfileIntW(L"update", L"proxy_chrome_download", 0,
                              GetIniPath().c_str()) != 0;
  download_source_ = ::GetPrivateProfileIntW(L"update", L"download_source", 1,
                                              GetIniPath().c_str());
  // Auto-detect architecture from the running process
  update_arch_ = DetectArch();

  // resolver_rules (域名映射): read the [resolver_rules] section.
  resolver_enabled_ =
      ::GetPrivateProfileIntW(L"resolver_rules", L"enabled", 0,
                              GetIniPath().c_str()) != 0;
  resolver_refresh_interval_ =
      ::GetPrivateProfileIntW(L"resolver_rules", L"refresh_interval", 0,
                              GetIniPath().c_str());
  resolver_max_total_ =
      ::GetPrivateProfileIntW(L"resolver_rules", L"max_total", 800,
                              GetIniPath().c_str());
  if (resolver_max_total_ <= 0) resolver_max_total_ = 800;
  resolver_subs_.clear();
  for (int i = 1; i <= 256; ++i) {
    std::wstring idx = std::to_wstring(i);
    std::wstring name =
        GetIniString(L"resolver_rules", L"sub_" + idx + L"_name", L"");
    if (name.empty()) continue;  // tolerate gaps left by removals
    ResolverSubscription sub;
    sub.name = std::move(name);
    sub.url = GetIniString(L"resolver_rules", L"sub_" + idx + L"_url", L"");
    sub.enabled = ::GetPrivateProfileIntW(
                      L"resolver_rules", (L"sub_" + idx + L"_enabled").c_str(),
                      0, GetIniPath().c_str()) != 0;
    std::wstring lr = GetIniString(L"resolver_rules",
                                   L"sub_" + idx + L"_last_refresh", L"0");
    sub.last_refresh = wcstoll(lr.c_str(), nullptr, 10);
    sub.rule_count = ::GetPrivateProfileIntW(
        L"resolver_rules", (L"sub_" + idx + L"_rule_count").c_str(), 0,
        GetIniPath().c_str());
    sub.cache = GetIniString(L"resolver_rules", L"sub_" + idx + L"_cache",
                             (L"sub_" + idx).c_str());
    resolver_subs_.push_back(std::move(sub));
  }

  // Sync the global debug-log gate so AddDebugLog() reflects the current ini
  // value (POST /api/config calls ReloadConfig() which re-runs LoadConfig()).
  g_enable_debug_log = debug_log_;
}

UpdateArch Config::DetectArch() {
#if defined(_M_ARM64)
  return UpdateArch::kARM64;
#elif defined(_M_X64)
  return UpdateArch::kX64;
#else
  return UpdateArch::kX86;
#endif
}

void Config::EnsureIniExists() {
  const std::wstring& path = GetIniPath();
  // File already present — nothing to do.
  if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return;
  }

  // Generate the full default INI with explanatory comments (modeled on
  // chrome++.ini). Defaults here MUST match the fallbacks in LoadConfig().
  // Note: ignore_policies, suppress_false_upgrade_notification and
  // show_password default to ON (1) per product decision.
  const wchar_t* kTemplate = LR"(; ChromeGreen 配置文件 (version.dll)，采用 INI 格式
; 以分号 (;) 开始的行是注释，不影响配置
; 开关类设置：0 关闭，1 开启
; 若选项留空或不存在，则使用默认配置；删除本文件会在下次启动时重新生成
; 路径支持 Windows 环境变量，例如 %appdata%
; 特别地，%app% 代表 chrome.exe 所在目录
; 若路径包含空格，请用双引号 ("") 包裹

[general]

; 用户数据目录（留空使用内置默认：%app%\..\Data；none 表示使用浏览器默认路径）
data_dir=%app%\..\Data

; 磁盘缓存目录（留空使用内置默认：%app%\..\Cache）
cache_dir=%app%\..\Cache

; 追加 Chromium 命令行开关，注意空格，不要换行
; 例如：command_line=--disable-features=OutdatedBuildDetector
command_line=

; 随浏览器启动的程序或命令
launch_on_startup=

; 浏览器退出时执行的程序或命令，多个用分号 (;) 分隔
launch_on_exit=

; 老板键：自定义快捷键，按下隐藏所有 Chrome 窗口，再次按下恢复（并随之静音/恢复静音）
; 可用按键：Ctrl、Alt、Shift、Win、F1-F12、0-9、A-Z、方向键等；填写格式示例：Ctrl+Alt+B
boss_key=

; 翻译快捷键：自定义网页翻译的快捷键，格式同老板键
translate_key=

; 打开新窗口的快捷键，格式同老板键；按下后打开一个新的 Chrome 窗口
open_new_window=

; 批量打开一组标签页的快捷键，格式同老板键；按下后按 [url_group] 列表逐个在新标签页打开
open_url_group=

; 免验证查看已保存的密码（无需系统登录密码即可显示密码明文）。0 关闭，1 开启
show_password=1

; 调试日志：开启时配置页显示“日志”导航与页面，并在后台记录运行日志。0 关闭（默认），1 开启
debug_log=0

; 屏蔽“不受支持的命令行标记”提示：开启后注入 --test-type（例如域名映射用到 --host-resolver-rules 时免弹警告）。注意副作用：部分安全/警告提示会被静默（含证书错误提示）。0 关闭（默认），1 开启
suppress_cmdline_warning=0

; 更新后自动打开配置页：应用内更新完成并重启 Chrome 时，自动在新标签页打开 ChromeGreen 配置页。0 关闭（默认），1 开启
open_config_after_update=0

; 强制启用 win32k 支持（仅当 ChromeGreen 导致 Chrome 启动崩溃时启用）。0 关闭，1 开启
win32k=0

; 阻止从注册表读取 Chrome 企业版策略。0 关闭，1 开启
ignore_policies=1

; 抑制 Chrome 错误的“已过期”升级提示（便携版无更新组件，该提示无法通过重启消除）。0 关闭，1 开启
suppress_false_upgrade_notification=1

; 界面主题：auto / light / dark
theme=auto

; 界面语言：auto / zh-CN / en
language=auto

[keymapping]
; 按键映射：源按键=目标按键 或 源按键=command:命令ID
; 按键同老板键；命令ID 见 chromium chrome_command_ids.h
; 示例：F2=Ctrl+PageUp（F2 切换上一个标签页）
; 示例：Alt+Shift+S=command:40015（打开设置）

[update]
; 更新通道：stable / beta / dev / canary
channel=stable
; 是否自动检查更新。0 关闭，1 开启
auto_check=1
; 自动检查间隔（小时）
check_interval=24
; 发现新版本是否自动下载。0 关闭，1 开启
auto_download=0
; 保留安装包。0 关闭，1 开启
keep_installer=0
; 保留旧版本。0 关闭，1 开启
keep_old_versions=0
; 更新代理地址（留空不使用）
proxy=
; 代理类型：HTTP / SOCKS5 / GH_PROXY（GH_PROXY 通过公共镜像加速 GitHub 访问，既代理自更新下载也代理版本检查，Chrome 下载不走代理）
proxy_type=HTTP
; 下载 Chrome 时也走代理。0 关闭，1 开启（GH_PROXY 类型下此选项无效）
proxy_chrome_download=0
; 下载源：0=edgedl 1=dl.google.com(默认) 2=www.google.com 3=redirector.gvt1.com
download_source=1

; 域名映射（仅作用于 Chrome 自身，不影响系统 hosts 文件或其它程序）
; 通过 Chromium --host-resolver-rules 在启动参数注入，需重启 Chrome 生效
[resolver_rules]
; 总开关。0 关闭，1 开启
enabled=0
; 定时刷新间隔（小时），0 = 仅手动刷新
refresh_interval=0
; 规则总数硬上限（命令行长度限制），超出则无法添加/刷新订阅
max_total=800

; 订阅源（编号段，sub_N_name/url/enabled/last_refresh/rule_count/cache）
; 添加订阅后由程序自动写入，无需手填；下方仅为格式示例
;sub_1_name=GitHub Hosts
;sub_1_url=https://raw.githubusercontent.com/example/hosts/master/hosts
;sub_1_enabled=1
;sub_1_last_refresh=0
;sub_1_rule_count=0
;sub_1_cache=sub_1

; 标签页增强（移植自 chrome_plus tabbookmark，在配置页“标签页增强配置”中设置）
[tabs]
; 保留至少一个标签页（永不关闭最后一个标签）。0 关闭，1 开启
keep_last_tab=1
; 双击标签关闭。0 关闭，1 开启
double_click_close=1
; 右键标签关闭（按住 Shift 显示原菜单）。0 关闭，1 开启
right_click_close=0
; 鼠标滚轮在标签栏上切换标签页。0 关闭，1 开启
wheel_tab=1
; 按住右键时滚轮切换标签页。0 关闭，1 开启
wheel_tab_when_press_rbutton=1
; 悬停（停留）在标签上激活它。0 关闭，1 开启
hover_tab=0
; 悬停激活延迟（毫秒，0-5000），仅 hover_tab=1 时生效
hover_tab_delay=400
; 地址栏回车在新标签打开网址：0 关闭，1 = Alt+Enter，2 = Shift+Alt+Enter
open_url_new_tab=0
; 书签在新标签打开：0 关闭，1 = 中键+Shift 点击，2 = 中键点击
open_bookmark_new_tab=0
; 对标题包含下列任一名称（逗号分隔）的标签禁用以下新标签增强。0 关闭，1 开启
new_tab_disable=1
new_tab_disable_name=

; 批量打开的网址列表（每行一个，配合上方 open_url_group 快捷键使用）
; 此处仅作示例，取消注释并填写后，按下该快捷键会逐个在新标签页打开
[url_group]
;1=https://www.google.com
;2=https://www.bing.com
)";

  // Normalize line endings to CRLF (the source raw string uses LF).
  std::wstring content(kTemplate);
  std::wstring crlf;
  crlf.reserve(content.size() + content.size() / 8);
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == L'\n' && (i == 0 || content[i - 1] != L'\r')) {
      crlf += L'\r';
    }
    crlf += content[i];
  }

  // Write UTF-16LE with BOM so the file stays Unicode across machines and is
  // safe for non-ASCII (e.g. Chinese) paths.
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  const BYTE bom[2] = {0xFF, 0xFE};
  DWORD written = 0;
  ::WriteFile(h, bom, 2, &written, nullptr);
  ::WriteFile(h, crlf.data(),
              static_cast<DWORD>(crlf.size() * sizeof(wchar_t)), &written,
              nullptr);
  ::CloseHandle(h);
}

void Config::LoadKeyMappings() {
  // Reset first: LoadConfig() is also called on reload (POST /api/config), so
  // stale entries must be dropped — otherwise an emptied [keymapping] section
  // (which returns chars_read==0) would leave the old mappings lingering in
  // memory and the config page would re-show them after a "clear" save.
  key_mappings_.clear();
  std::vector<wchar_t> buffer(4096);
  const DWORD chars_read = ::GetPrivateProfileSectionW(
      L"keymapping", buffer.data(), static_cast<DWORD>(buffer.size()),
      GetIniPath().c_str());

  if (chars_read == 0) {
    return;
  }

  const wchar_t* current = buffer.data();
  while (*current != L'\0') {
    const std::wstring_view line(current);
    current += line.length() + 1;

    const auto eq_pos = line.find(L'=');
    if (eq_pos == std::wstring_view::npos || eq_pos == 0) {
      continue;
    }

    std::wstring_view key = line.substr(0, eq_pos);
    std::wstring_view value = line.substr(eq_pos + 1);

    while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) {
      key.remove_suffix(1);
    }
    while (!value.empty() &&
           (value.front() == L' ' || value.front() == L'\t')) {
      value.remove_prefix(1);
    }

    if (!key.empty() && !value.empty()) {
      key_mappings_.emplace_back(std::wstring(key), std::wstring(value));
    }
  }
}

std::optional<std::wstring> Config::LoadDirPath(const std::wstring& dir_type) {
  std::wstring path = CanonicalizePath(GetAppDir() + L"\\..\\" + dir_type);
  std::wstring dir_key = dir_type + L"_dir";
  std::wstring dir_buffer = GetIniString(L"general", dir_key, path);

  if (dir_buffer == L"none") {
    return std::nullopt;
  }

  if (dir_buffer.empty()) {
    dir_buffer = path;
  }

  std::wstring expanded_path = ExpandEnvironmentPath(dir_buffer);
  ReplaceStringInPlace(expanded_path, L"%app%", GetAppDir());
  return GetAbsolutePath(expanded_path);
}

int Config::LoadHoverTabDelay() {
  constexpr int kDefaultDelayMs = 400;
  constexpr int kMaxDelayMs = 5000;
  const int delay = ::GetPrivateProfileIntW(
      L"tabs", L"hover_tab_delay", kDefaultDelayMs, GetIniPath().c_str());
  if (delay < 0 || delay > kMaxDelayMs) {
    return kDefaultDelayMs;
  }
  return delay;
}

int Config::LoadOpenUrlNewTabMode() {
  return ::GetPrivateProfileIntW(L"tabs", L"open_url_new_tab", 0,
                                 GetIniPath().c_str());
}

int Config::LoadBookmarkNewTabMode() {
  return ::GetPrivateProfileIntW(L"tabs", L"open_bookmark_new_tab", 0,
                                 GetIniPath().c_str());
}

const Config& config = Config::Instance();
