# ChromeGreen

[![License](https://img.shields.io/github/license/libsgh/chrome_green?style=for-the-badge)](LICENSE)
[![Latest Release](https://img.shields.io/github/v/release/libsgh/chrome_green?style=for-the-badge&logo=github)](https://github.com/libsgh/chrome_green/releases)
[![Downloads](https://img.shields.io/github/downloads/libsgh/chrome_green/total?style=for-the-badge&logo=github)](https://github.com/libsgh/chrome_green/releases)
[![Platform](https://img.shields.io/badge/platform-Windows-0078D4?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/libsgh/chrome_green)
[![Language](https://img.shields.io/badge/language-C%2B%2B-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://github.com/libsgh/chrome_green)

简体中文 | [English](README.md)

ChromeGreen 是一个 `version.dll` 注入项目，**主要实现目标是便携化（绿化）及在线更新**。它让你把 Chrome 变成自包含、可随身携带的应用，并通过**内置的在线更新**保持便携版常新，还能在**内置配置页面**上直接修改所有设置——无需手动编辑文件。

## 核心功能

- **便携（绿化）。** 重定向用户数据、缓存以及机器标识（计算机名、卷序列号、DPAPI 加密），让 Chrome 可以从任意目录或 U 盘运行——无需安装、不绑定注册表。
- **便携 Chrome 的在线更新。** 内置更新器可就地下载并替换便携版 Chrome，且保留你的 `Data` 与 `Cache`，更新永远不会清空你的个人配置。
- **页面内配置。** 注入的 DLL 自带一个配置页面，你可以在上面直接修改更新通道、代理、数据/缓存路径、快捷键等，**无需手改配置文件**。
- **配置老板键**。以隐藏和恢复 Chrome 窗口，并随之静音与恢复静音。
- 配置网页翻译快捷键
- 通过 `keymapping` 将按键映射到其它快捷键或 Chrome 命令 ID
- 通过 `data_dir` 和 `cache_dir` 控制便携化数据路径
- 通过 `command_line` 追加 Chromium 启动参数
- 通过 `launch_on_startup` 和 `launch_on_exit` 在启动或退出时执行程序或命令
- 通过 `ignore_policies` 忽略企业策略
- 仅在 ChromeGreen 自身导致启动崩溃时再考虑启用 `win32k` 兜底选项。
- 通过 `suppress_false_upgrade_notification` 抑制便携版上错误的“已过期”升级提示。

## 获取

[Release](https://github.com/libsgh/chrome_green/releases)

## 安装
1. 从 Releases 页面下载 `version.dll`。
2. 将 `version.dll` 放在 `chrome.exe` 同一目录下（仅在需要自定义设置时再放入 `chrome_green.ini`）。
3. 打开配置页面——在 `chrome://settings/help` 中点击 **ChromeGreen** 链接（推荐，链接已按安装目录自动指向本实例的端口）。注意：配置页端口是按安装目录派生的，多个便携 Chrome 各自使用不同端口，请勿手动访问固定的 `127.0.0.1:8090`，否则可能连到另一个实例的服务端。
4. 完成，Chrome 已是便携版。

## 配置

配置项是**可选的**。开箱即用时，ChromeGreen 会以合理的默认值使 Chrome 便携（用户数据在 `../Data`，缓存在 `../Cache`）。

- 大多数设置都可以在**内置配置页面**上实时修改——无需编辑文件。
- 完整的可配置项及默认值见 [`src/chrome_green.ini`](src/chrome_green.ini)，其中每个选项都有行内说明。

## 源码构建

前置条件：
- Windows 10/11
- 安装 [LLVM](https://llvm.org/) 并包含 `clang-cl`（例如 20.x）——构建使用 clang-cl 工具链，而非 MSVC
- [xmake](https://xmake.io/)

直接运行 [`scripts/build.bat`](scripts/build.bat) 即可完成配置与构建（它通过 xmake 调用 clang-cl 工具链）。

## 许可证
基于 [Chrome++ Next](https://github.com/Bush2021/chrome_plus) 二次开发。使用 [GPL-3.0](LICENSE) 许可证。

## 致谢
- 原作者 [Shuax](https://github.com/shuax/)
- [Chrome++ Next](https://github.com/Bush2021/chrome_plus) 维护者和贡献者
