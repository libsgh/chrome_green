#include "pakpatch.h"

#include <windows.h>

#include <sddl.h>

#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>

#include "detours.h"

#include "appid.h"
#include "pakfile.h"
#include "update.h"
#include "utils.h"
#include "version.h"
#include "httpserver.h"

namespace {
#if defined(_M_ARM64)
#define BUILD_ARCH " (ARM64)"
#elif defined(_M_X64)
#define BUILD_ARCH " (64-bit)"
#else
#define BUILD_ARCH " (32-bit)"
#endif

static HANDLE resources_pak_map = nullptr;

static auto RawCreateFileMapping = CreateFileMappingW;
static auto RawMapViewOfFile = MapViewOfFile;

// `PakPatch()` runs in the browser and in every renderer (see the loader in
// chrome++.cc: with in-process WebUI resource loading the renderer serves
// `chrome://settings` from its own pak mapping, so it must patch that mapping
// itself). Re-running the content scan in each renderer inflated every gzip
// entry up to the target and re-deflated the patched one at level 9, on the
// renderer main thread during `PreSandboxStartup` -- a freeze on every new
// tab and cross-site navigation. Instead the browser scans once and hands the
// result to its children through two inherited environment variables: the
// target's pak resource id, so a renderer that has to decompress inflates
// exactly one entry, and the name of a read-only section holding the
// browser's already-patched entry, which a renderer applies with a single
// memcpy and no decompression at all.
constexpr wchar_t kPakTargetIdEnv[] = L"CHROME_GREEN_PAK_RES_ID";
constexpr wchar_t kPakBlobEnv[] = L"CHROME_GREEN_PAK_BLOB";

struct PakBlobHeader {
  uint32_t resource_id;
  uint32_t length;
};

// Keeps the published section alive for the browser's lifetime so child
// processes can open it by name.
static HANDLE published_blob_section = nullptr;

// The loader calls `PakPatch()` only in the browser and in `--type=renderer`
// children (chrome++.cc `Loader`), so no `-type=` switch means the browser.
bool IsBrowserProcess() {
  return !wcsstr(GetCommandLineW(), L"-type=");
}

uint16_t GetPakTargetId() {
  wchar_t value[8];
  DWORD len = GetEnvironmentVariableW(kPakTargetIdEnv, value, ARRAYSIZE(value));
  if (len == 0 || len >= ARRAYSIZE(value)) {
    return 0;
  }
  wchar_t* end = nullptr;
  unsigned long id = wcstoul(value, &end, 10);
  if (end == value || id == 0 || id > 0xFFFF) {
    return 0;
  }
  return static_cast<uint16_t>(id);
}

// Browser side: copy the patched slot into a named read-only section and
// publish the name through the environment. Read-only is load-bearing: the
// bytes feed the privileged settings WebUI in every renderer, so a writable
// section would be an HTML injection vector into `chrome://settings`. The
// DACL grants read to Everyone and to RestrictedCode (the renderer's
// restricting SID), so a renderer's pre-lockdown token can open the section
// at the same point it maps the pak.
void PublishPatchedEntry(uint8_t* buffer, uint16_t resource_id) {
  // After an in-app restart the new browser inherits the old browser's
  // section name, which dies with that process; drop it and republish under
  // this pid.
  SetEnvironmentVariableW(kPakBlobEnv, nullptr);

  const auto slot = FindResourceSlot(buffer, resource_id);
  if (!slot) {
    return;
  }

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GR;;;WD)(A;;GR;;;RC)", SDDL_REVISION_1,
          &sa.lpSecurityDescriptor, nullptr)) {
    return;
  }

  const std::wstring name =
      L"Local\\ChromeGreenPakBlob_" + std::to_wstring(GetCurrentProcessId());
  const DWORD size = static_cast<DWORD>(sizeof(PakBlobHeader) + slot->length);
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                      0, size, name.c_str());
  const DWORD create_error = GetLastError();
  LocalFree(sa.lpSecurityDescriptor);
  if (!section) {
    return;
  }
  // A pre-existing name means another process squatted it; leave children on
  // the decompress fallback rather than trust its contents.
  if (create_error == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return;
  }

  auto* view =
      static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size));
  if (!view) {
    CloseHandle(section);
    return;
  }
  auto* header = reinterpret_cast<PakBlobHeader*>(view);
  header->resource_id = resource_id;
  header->length = slot->length;
  memcpy(view + sizeof(PakBlobHeader), buffer + slot->offset, slot->length);
  UnmapViewOfFile(view);

  published_blob_section = section;
  SetEnvironmentVariableW(kPakBlobEnv, name.c_str());
  DebugLog(L"PakPatch: published resource {} ({} bytes) as {}", resource_id,
           slot->length, name);
}

// Renderer fast path: overwrite this process's copy-on-write pak view with
// the browser's already-patched bytes. The slot is re-derived from this
// process's own pak index and the lengths must match, so a section built from
// a different pak is rejected and the copy cannot write outside the slot.
bool ApplyPatchedEntry(uint8_t* buffer) {
  wchar_t name[64];
  DWORD len = GetEnvironmentVariableW(kPakBlobEnv, name, ARRAYSIZE(name));
  if (len == 0 || len >= ARRAYSIZE(name)) {
    return false;
  }

  HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!section) {
    return false;
  }

  bool applied = false;
  if (const auto* view = static_cast<const uint8_t*>(
          MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0))) {
    const auto* header = reinterpret_cast<const PakBlobHeader*>(view);
    std::optional<PakResourceSlot> slot;
    if (header->resource_id <= 0xFFFF) {
      slot =
          FindResourceSlot(buffer, static_cast<uint16_t>(header->resource_id));
    }
    if (slot && slot->length == header->length) {
      memcpy(buffer + slot->offset, view + sizeof(PakBlobHeader), slot->length);
      applied = true;
      DebugLog(L"PakPatch: applied published resource {}", header->resource_id);
    }
    UnmapViewOfFile(view);
  }
  CloseHandle(section);
  return applied;
}

// The #172 settings-page injection, run on each candidate decompressed pak
// entry until it finds the one holding the settings-about-page HTML.
bool PatchSettingsHtml(uint8_t* begin, uint32_t size, size_t& new_len) {
  BYTE search_start[] = R"(</settings-about-page>)";
  auto match = SearchMemory(
      std::span<uint8_t>(begin, size),
      std::span<const uint8_t>(search_start, sizeof(search_start) - 1));
  if (match.empty()) {
    return false;
  }

  // Compress the HTML for writing patch information.
  std::string html(reinterpret_cast<char*>(begin), size);
  compression_html(html);

  // Hide the update status area so Chrome's native update check (which always
  // fails on a portable build — no Google Update COM service) is never shown.
  // Chrome ≤150 uses Polymer bindings, Chrome ≥151 uses Lit bindings.
  // Both sets are tried — the ones that don't match are no-ops.
  //
  // CRITICAL: We replace the whole binding with an inline style, NOT a static
  // attribute. Reason: Lit parses templates at module load time. A `?attr`
  // boolean binding MUST contain a ${...} interpolation; if we leave `?hidden`
  // with a static value ("?hidden=\"true\""), Lit no longer treats it as a
  // binding and the browser ignores the unknown `?hidden` attribute entirely
  // → the element stays visible. An inline style works regardless of framework.
  ReplaceStringInPlace(html, R"(hidden="[[!showUpdateStatus_]]")",
                       R"(style="display:none!important")");
  ReplaceStringInPlace(html,
                       R"(hidden="[[!shouldShowIcons_(showUpdateStatus_)]]")",
                       R"(style="display:none!important")");
  // Lit (Chrome ≥151): ?hidden="${...}" boolean attribute bindings
  ReplaceStringInPlace(html, R"(?hidden="${!this.showUpdateStatus_}")",
                       R"(style="display:none!important")");
  ReplaceStringInPlace(html, R"(?hidden="${!this.shouldShowIcons_()}")",
                       R"(style="display:none!important")");

  // Build the update status string for the inline badge.
  // All statuses are clickable links to the ChromeGreen config page so the
  // user can always see the live status there (PAK patch bakes a snapshot
  // at startup that may become stale).
  auto state = GetUpdateStateSnapshot();
  std::string update_status;
  // Per-install port so two portable Chromes don't share one server (see
  // GetConfigPort). The link baked here must match the port the HTTP server
  // binds in this same process.
  std::string config_url = "http://127.0.0.1:" + std::to_string(GetConfigServerPort());
  switch (state.state) {
    case UpdateState::kChecking:
      update_status = R"(<a target="_blank" href=")" + std::string(config_url) +
                       R"(">Checking...</a>)";
      break;
    case UpdateState::kAvailable:
      update_status = R"(<a target="_blank" href=")" + std::string(config_url) +
                       R"(">Update: )" + state.latest_version + "</a>";
      break;
    case UpdateState::kDownloading:
      update_status = R"(<a target="_blank" href=")" + std::string(config_url) +
                       R"(">Downloading: )" + std::to_string(state.download_progress) + "%</a>";
      break;
    case UpdateState::kReady:
      update_status = R"(<a target="_blank" href=")" + std::string(config_url) +
                       R"(">Update ready &mdash; click to restart</a>)";
      break;
    case UpdateState::kError:
      update_status = R"(<a target="_blank" href=")" + std::string(config_url) +
                       R"(">Error</a>)";
      break;
    default:  // kIdle
      if (!state.latest_version.empty() &&
          state.latest_version != state.current_version) {
        update_status = R"(<a target="_blank" href=")" + std::string(config_url) +
                         R"(">Update: )" + state.latest_version + "</a>";
      } else {
        update_status = R"(<a target="_blank" href=")" + std::string(config_url) +
                         R"(">Up to date</a>)";
      }
      break;
  }

  // Single line: Powered by ChromeGreen (clickable) version (arch)
  std::string info_line = "Powered by <a target=\"_blank\" href=\"" + config_url +
                          "\">ChromeGreen</a> ";
  info_line += RELEASE_VER_STR;
  info_line += BUILD_ARCH;

  // Inject the branding line after the version string.
  // Chrome ≤150 (Polymer): {aboutBrowserVersion}</div>
  // Chrome ≥151 (Lit):     $i18n{aboutBrowserVersion}</div>
  // Try Lit first — the Polymer pattern is a substring of the Lit pattern
  // ({aboutBrowserVersion}</div> appears inside $i18n{aboutBrowserVersion}</div>),
  // so we must match the longer string first to avoid a partial replacement.
  const std::string lit_target = R"($i18n{aboutBrowserVersion}</div>)";
  const std::string polymer_target = R"({aboutBrowserVersion}</div>)";
  const std::string inject = R"(<div class="secondary">)" + info_line + R"(</div>)";

  if (html.find(lit_target) != std::string::npos) {
    ReplaceStringInPlace(html, lit_target, lit_target + inject);
  } else {
    ReplaceStringInPlace(html, polymer_target, polymer_target + inject);
  }

  if (html.length() > size) {
    return false;
  }
  memcpy(begin, html.c_str(), html.length());
  new_len = html.length();
  return true;
}

// One flow per process kind: the browser locates and patches the entry
// itself, then publishes the id and the patched bytes for its children; a
// renderer takes the cheapest tier available -- published bytes (no
// decompression), targeted decompress (one entry), full content scan. Runs
// inside `MyMapViewOfFile` after both hooks have detached themselves, so the
// section create/open/map calls in the publish and apply helpers reach the
// real APIs, not our hooks.
void PatchResourcesPak(uint8_t* buffer) {
  const bool is_browser = IsBrowserProcess();
  if (!is_browser && ApplyPatchedEntry(buffer)) {
    return;
  }

  const uint16_t target_id = GetPakTargetId();
  uint16_t matched_id = TraversalGZIPFile(buffer, PatchSettingsHtml, target_id);
  if (matched_id == 0 && target_id != 0) {
    // The inherited id missed, so the pak was replaced (browser updated
    // between sessions); redo the full content scan.
    matched_id = TraversalGZIPFile(buffer, PatchSettingsHtml, 0);
  }

  if (is_browser && matched_id != 0) {
    SetEnvironmentVariableW(kPakTargetIdEnv,
                            std::to_wstring(matched_id).c_str());
    PublishPatchedEntry(buffer, matched_id);
  }
}

HANDLE WINAPI MyMapViewOfFile(_In_ HANDLE hFileMappingObject,
                              _In_ DWORD dwDesiredAccess,
                              _In_ DWORD dwFileOffsetHigh,
                              _In_ DWORD dwFileOffsetLow,
                              _In_ SIZE_T dwNumberOfBytesToMap) {
  if (hFileMappingObject == resources_pak_map) {
    // Modify it to be modifiable.
    LPVOID buffer =
        RawMapViewOfFile(hFileMappingObject, FILE_MAP_COPY, dwFileOffsetHigh,
                         dwFileOffsetLow, dwNumberOfBytesToMap);

    // No more hook needed.
    resources_pak_map = nullptr;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<LPVOID*>(&RawMapViewOfFile),
                 reinterpret_cast<void*>(MyMapViewOfFile));
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Unhook RawMapViewOfFile failed {}", status);
    }

    if (buffer) {
      PatchResourcesPak(static_cast<uint8_t*>(buffer));
    }

    return buffer;
  }

  return RawMapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
                          dwFileOffsetLow, dwNumberOfBytesToMap);
}

// Identify `resources.pak` where it is mapped, by querying the handle, rather
// than where it is opened. Chrome maps the pak with
// `CreateFileMapping`/`MapViewOfFile` (`base::MemoryMappedFile`), so this hook
// catches it directly in every process that loads it -- including the renderer,
// which opens the pak by path itself (`AddDataPackFromPath` in
// `ChromeMainDelegate::PreSandboxStartup`, before sandbox lockdown). The
// previous `CreateFile` hook existed only to record that handle so this hook
// could match it later; querying the handle here drops that second hook.
// `GetFileInformationByHandleEx(FileNameInfo)` reads the path of a handle the
// process already holds, which the sandbox permits -- it brokers new opens
// (`NtCreateFile`/`NtOpenFile`), not operations on existing handles.
// https://chromium.googlesource.com/chromium/src/+/main/docs/design/sandbox.md
bool IsResourcesPak(HANDLE hFile) {
  if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE) {
    return false;
  }
  alignas(FILE_NAME_INFO)
      BYTE buffer[sizeof(FILE_NAME_INFO) + MAX_PATH * 2 * sizeof(wchar_t)];
  auto* info = reinterpret_cast<FILE_NAME_INFO*>(buffer);
  if (!GetFileInformationByHandleEx(hFile, FileNameInfo, info,
                                    sizeof(buffer))) {
    return false;
  }
  std::wstring_view name(info->FileName,
                         info->FileNameLength / sizeof(wchar_t));
  return name.ends_with(L"resources.pak");
}

HANDLE WINAPI MyCreateFileMapping(_In_ HANDLE hFile,
                                  _In_opt_ LPSECURITY_ATTRIBUTES lpAttributes,
                                  _In_ DWORD flProtect,
                                  _In_ DWORD dwMaximumSizeHigh,
                                  _In_ DWORD dwMaximumSizeLow,
                                  _In_opt_ LPCTSTR lpName) {
  if (IsResourcesPak(hFile)) {
    // Force copy-on-write so the mapped view can be patched in memory.
    resources_pak_map =
        RawCreateFileMapping(hFile, lpAttributes, PAGE_WRITECOPY,
                             dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    // No more hook needed.
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<LPVOID*>(&RawCreateFileMapping),
                 reinterpret_cast<void*>(MyCreateFileMapping));
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Unhook RawCreateFileMapping failed {}", status);
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<LPVOID*>(&RawMapViewOfFile),
                 reinterpret_cast<void*>(MyMapViewOfFile));
    status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Hook RawMapViewOfFile failed {}", status);
    }

    return resources_pak_map;
  }
  return RawCreateFileMapping(hFile, lpAttributes, flProtect, dwMaximumSizeHigh,
                              dwMaximumSizeLow, lpName);
}

}  // namespace

void PakPatch() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<LPVOID*>(&RawCreateFileMapping),
               reinterpret_cast<void*>(MyCreateFileMapping));
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"Hook RawCreateFileMapping failed {}", status);
  }
}
