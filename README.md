# ChromeGreen

[![License](https://img.shields.io/github/license/libsgh/chrome_green?style=for-the-badge)](LICENSE)
[![Latest Release](https://img.shields.io/github/v/release/libsgh/chrome_green?style=for-the-badge&logo=github)](https://github.com/libsgh/chrome_green/releases)
[![Downloads](https://img.shields.io/github/downloads/libsgh/chrome_green/total?style=for-the-badge&logo=github)](https://github.com/libsgh/chrome_green/releases)
[![Platform](https://img.shields.io/badge/platform-Windows-0078D4?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/libsgh/chrome_green)
[![Language](https://img.shields.io/badge/language-C%2B%2B-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://github.com/libsgh/chrome_green)

English | [简体中文](README.zh-CN.md)

ChromeGreen is a `version.dll` injection for Google Chrome whose main goal is **portability (绿化) and in-place online updates**. It turns Chrome into a self-contained, carry-anywhere app, keeps that portable install current with a **built-in online updater**, and lets you tweak every setting from a **built-in config page** — no file editing required.

## Core Features

- **Portable by design (绿化).** Redirects user data, cache, and machine-specific identifiers (computer name, volume serial, DPAPI encryption) so Chrome runs from any folder or USB drive — no installation, no registry binding.
- **Online update for portable Chrome.** A built-in updater downloads and replaces the portable Chrome build in place; your `Data` and `Cache` are preserved, so updating never wipes your profile.
- **In-page configuration.** The injected DLL serves a built-in config page where you can change the update channel, proxy, data/cache paths, hotkeys, and more — no need to hand-edit config files.
- Configure a boss key to hide and restore Chrome windows, muting and unmuting audio at the same time.
- Configure a web-page translation hotkey.
- Map keys to other shortcuts or Chrome command IDs via `keymapping`.
- Control the portable data paths via `data_dir` and `cache_dir`.
- Append Chromium launch flags via `command_line`.
- Run programs or commands on startup or exit via `launch_on_startup` and `launch_on_exit`.
- Ignore enterprise policies via `ignore_policies`.
- Only enable the `win32k` fallback option if ChromeGreen itself causes a startup crash.
- Suppress the bogus "out of date" upgrade prompt on the portable build via `suppress_false_upgrade_notification`.

## Download

[Release](https://github.com/libsgh/chrome_green/releases)

## Installation
1. Download `version.dll` from the Releases page.
2. Place `version.dll` in the same directory as `chrome.exe` (add `chrome_green.ini` only if you need custom settings).
3. Open the config page — click the **ChromeGreen** link on `chrome://settings/help` (recommended; the link is auto-derived per install). Note: the config port is derived from the install directory, so multiple portable Chromes each use a different port — do not manually navigate to a fixed `127.0.0.1:8090`, or you may hit another instance's server.
4. Done — Chrome is now portable.

## Configuration

Configuration is **optional**. Out of the box ChromeGreen makes Chrome portable with sensible defaults (user data in `../Data`, cache in `../Cache`).

- Most settings can be changed live from the **built-in config page** — no file editing needed.
- For the full list of tunables and their defaults, see [`src/chrome_green.ini`](src/chrome_green.ini); every option is documented inline there.

## Build from Source

Requirements:
- Windows 10/11
- [LLVM](https://llvm.org/) with `clang-cl` (e.g. 20.x) — the build uses the clang-cl toolchain, not MSVC
- [xmake](https://xmake.io/)

Run [`scripts/build.bat`](scripts/build.bat) to configure and build (it drives xmake with the clang-cl toolchain).

## License
Forked from [Chrome++ Next](https://github.com/Bush2021/chrome_plus). Licensed under [GPL-3.0](LICENSE).

## Credits
- Original author [Shuax](https://github.com/shuax/)
- [Chrome++ Next](https://github.com/Bush2021/chrome_plus) maintainers and contributors
