// Shared application store for the ChromeGreen config page.
//
// All reactive state and actions used across tabs live here as module-level
// singletons, so every tab component imports the *same* instances (no prop
// drilling). App.vue is just the shell (header / nav / toast / lifecycle) and
// delegates each tab's content to a dedicated component under components/tabs/.
import {
  computed,
  reactive,
  ref,
  watch,
} from "vue";
import { api } from "./api.js";
import { setLocale, t } from "./i18n.js";

const GITHUB_URL = "https://github.com/libsgh/chrome_green";

// --- Reactive state (created once at module load) ---
const status = reactive({
  state: "idle",
  current_version: "",
  latest_version: "",
  channel: "stable",
  arch: "x64",
  download_progress: 0,
  download_size: 0,
  downloaded_bytes: 0,
  download_speed: 0,
  download_eta: 0,
  error_message: "",
  last_check_time: 0,
  auto_check: false,
  auto_download: false,
  proxy: "",
  proxy_type: "HTTP",
  proxy_chrome_download: false,
  chrome_green_version: "",
  self_latest_version: "",
  self_download_size: 0,
  self_download_progress: 0,
  self_update_ready: false,
  self_release_notes: "",
  self_has_update: false,
  self_downloading: false,
  has_local_package: false,
  has_pending_swap: false,
});

const settings = reactive({
  channel: "stable",
  auto_check: false,
  check_interval: 24,
  auto_download: false,
  keep_installer: false,
  keep_old_versions: false,
  proxy: "",
  proxy_type: "HTTP",
  proxy_chrome_download: false,
  download_source: 1,
  theme: "auto",
  language: "auto",
  // general (portable / launch / hotkeys)
  data_dir: "%app%\\..\\Data",
  cache_dir: "%app%\\..\\Cache",
  command_line: "",
  launch_on_startup: "",
  launch_on_exit: "",
  translate_key: "",
  boss_key: "",
  // action hotkeys (open new window / batch-open URL group)
  open_new_window: "",
  open_url_group: "",
  url_group: "",
  win32k: false,
  ignore_policies: false,
  suppress_false_upgrade_notification: false,
  show_password: false,
  debug_log: false,
  // tabs (ported from chrome_plus tabbookmark)
  keep_last_tab: true,
  double_click_close: true,
  right_click_close: false,
  wheel_tab: true,
  wheel_tab_when_press_rbutton: true,
  hover_tab: false,
  hover_tab_delay: 400,
  open_url_new_tab: 0,
  open_bookmark_new_tab: 0,
  new_tab_disable: true,
  new_tab_disable_name: "",
  key_mappings: "",
});

// Key mappings editor: a list of {src, dst} rows. Serialized to the
// newline-joined "src=dst" string stored in settings.key_mappings.
const keyMappings = reactive([]);

const checking = ref(false);
const downloading = ref(false);
const selfChecking = ref(false);
// Driven by the backend's self_downloading flag (pushed over SSE), not a local
// flag. The download runs on a backend thread, so a local flag would reset to
// false the moment the HTTP call returned, even while the download is still
// going and the UI would lose the progress.
const selfDownloading = computed(() => status.self_downloading === true);
// Whether at least one explicit update check (Chrome or ChromeGreen) has run
// since the page opened. Until then the status badge reads "未检查" (Not checked).
const hasChecked = ref(false);
const themeMode = ref(localStorage.getItem("chrome_green_theme") || "auto");
const langMode = ref(localStorage.getItem("chrome_green_lang") || "auto");
const logs = ref([]);

// Toast notification (shown after an explicit save from the Other tab)
const toast = reactive({ show: false, message: "", type: "success" });
let toastTimer = null;
function showToast(message, type = "success") {
  toast.message = message;
  toast.type = type;
  toast.show = true;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    toast.show = false;
  }, 2600);
}

const logCopied = ref(false);
const showThemeDropdown = ref(false);
const showLangDropdown = ref(false);
const activeTab = ref("status");
const allTabs = [
  { id: "status", label: "nav_status", icon: "status" },
  { id: "settings", label: "nav_settings", icon: "settings" },
  { id: "proxy", label: "nav_proxy", icon: "network" },
  { id: "tabs", label: "nav_tabs", icon: "tabs" },
  { id: "logs", label: "nav_logs", icon: "logs" },
  { id: "other", label: "nav_other", icon: "sliders" },
];
// The Logs tab and page are only available when debug logging is enabled.
const tabs = computed(() => {
  if (settings.debug_log) return allTabs;
  return allTabs.filter((t) => t.id !== "logs");
});
// If debug logging is turned off while the Logs tab is the active view,
// bounce the user back to the Status tab.
watch(
  () => settings.debug_log,
  (enabled) => {
    if (!enabled && activeTab.value === "logs") activeTab.value = "status";
  },
);

// Tools tab state
const tools = reactive({ desktop_shortcut: false });
const shortcutBusy = ref(false);
const toolMessage = ref("");
const toolMessageOk = ref(true);

// Default (non-portable) Chrome data directory cleaner.
const chromeData = reactive({ exists: false, empty: true, entry_count: 0 });
const chromeDataBusy = ref(false);
const chromeDataMessage = ref("");
const chromeDataMessageOk = ref(true);
let toolPollTimer = null;
const channelOptions = computed(() => [
  { value: "stable", label: t("channel_stable") },
  { value: "beta", label: t("channel_beta") },
  { value: "dev", label: t("channel_dev") },
  { value: "canary", label: t("channel_canary") },
]);
const checkIntervalOptions = computed(() => [
  { value: 1, label: t("interval_1h") },
  { value: 4, label: t("interval_4h") },
  { value: 8, label: t("interval_8h") },
  { value: 12, label: t("interval_12h") },
  { value: 24, label: t("interval_24h") },
  { value: 72, label: t("interval_72h") },
  { value: 168, label: t("interval_168h") },
]);
const proxyTypeOptions = computed(() => [
  { value: "HTTP", label: "HTTP" },
  { value: "SOCKS5", label: "SOCKS5" },
  { value: "GH_PROXY", label: "GH_PROXY" },
]);
let logPollTimer = null;
const logPollingInterval = 5000;
let retryCount = 0;

// Live status via Server-Sent Events. The config page keeps one
// EventSource('/api/stream') open; the backend pushes the status JSON
// (~every 300ms) so the progress bar / speed / ETA update in real time
// without any client-side polling. EventSource auto-reconnects on drop.
let statusSource = null;
// One-shot callback invoked when the backend reports a captured hotkey via the
// SSE 'key_capture' event. Only the currently-recording KeyInput registers it.
let keyCaptureCallback = null;
function setKeyCaptureCallback(cb) {
  keyCaptureCallback = cb;
}
function startStatusStream() {
  if (statusSource) return;
  statusSource = new EventSource("/api/stream");
  statusSource.onmessage = (e) => {
    try {
      Object.assign(status, JSON.parse(e.data));
      retryCount = 0;
    } catch (_) {}
  };
  // Backend-assisted hotkey capture event.
  statusSource.addEventListener("key_capture", (e) => {
    if (!keyCaptureCallback) return;
    try {
      const data = JSON.parse(e.data);
      const cb = keyCaptureCallback;
      keyCaptureCallback = null;
      cb(data);
    } catch (_) {}
  });
  statusSource.onerror = () => {
    // EventSource reconnects automatically. As a safety net, if the stream
    // is unavailable we fall back to a one-shot fetch a few times so the
    // page still reflects state changes from user actions.
    retryCount++;
    if (retryCount <= 5) refreshStatus().catch(() => {});
  };
}
function stopStatusStream() {
  if (statusSource) {
    statusSource.close();
    statusSource = null;
  }
}

function startLogPolling() {
  if (logPollTimer) return;
  logPollTimer = setInterval(loadLogs, logPollingInterval);
}

function stopLogPolling() {
  if (!logPollTimer) return;
  clearInterval(logPollTimer);
  logPollTimer = null;
}

function updatePolling() {
  // Status updates come from the SSE stream (startStatusStream), so there
  // is no client-side status polling. We only manage the logs / tools tabs.
  const shouldPollLogs = activeTab.value === "logs";
  const shouldPollTools = activeTab.value === "other";

  if (shouldPollLogs) {
    loadLogs().catch(() => {});
    startLogPolling();
  } else stopLogPolling();

  if (shouldPollTools) {
    refreshToolsStatus().catch(() => {});
    startToolsPolling();
  } else stopToolsPolling();
}

function startToolsPolling() {
  if (toolPollTimer) return;
  toolPollTimer = setInterval(refreshToolsStatus, 5000);
}
function stopToolsPolling() {
  if (!toolPollTimer) return;
  clearInterval(toolPollTimer);
  toolPollTimer = null;
}

const version = computed(() => status.chrome_green_version || "1.0.0");
const githubUrl = GITHUB_URL;
const themeClass = computed(() => {
  if (themeMode.value === "dark") return "dark";
  if (themeMode.value === "light") return "";
  return window.matchMedia("(prefers-color-scheme: dark)").matches
    ? "dark"
    : "";
});
const selfHasUpdate = computed(() => {
  // The local-vs-remote comparison now lives on the backend; the flag is
  // derived there (published release > locally compiled RELEASE_VER_STR) and
  // delivered via the check response and the SSE status stream.
  return status.self_has_update === true;
});
const isAutoChecking = computed(
  () => checking.value || status.state === "checking",
);

// Remaining-time label for the download progress row. While downloading, an
// ETA of <=0 means "almost done" (not "complete", which only applies once the
// state leaves downloading). That reads better than a bare "0".
const etaText = computed(() => {
  if (status.state !== "downloading") return "";
  const s = status.download_eta;
  if (s < 0) return t("eta_unknown");
  if (s <= 0) return t("eta_almost");
  return formatEta(s);
});

const statusLabel = computed(() => {
  if (!hasChecked.value) return t("states.not_checked");
  const labels = {
    idle: "states.idle",
    checking: "states.checking",
    available: "states.available",
    downloading: "states.downloading",
    ready: "states.ready",
    applying: "states.applying",
    pending_apply: "states.pending_apply",
    error: "states.error",
  };
  let label = t(labels[status.state] || "states.idle");
  if (
    status.latest_version &&
    (status.state === "available" ||
      status.state === "downloading" ||
      status.state === "ready" ||
      status.state === "pending_apply")
  ) {
    label += " — " + status.latest_version;
  }
  return label;
});
const statusBadgeClass = computed(() => {
  if (!hasChecked.value) return "badge badge-neutral";
  const variants = {
    idle: "badge-success",
    checking: "badge-info",
    available: "badge-warning",
    downloading: "badge-info",
    ready: "badge-success",
    applying: "badge-info",
    pending_apply: "badge-info",
    error: "badge-error",
  };
  const variant = variants[status.state] || "badge-neutral";
  return "badge " + variant;
});

// Tools status badge: green when done, neutral when not.
function toolBadgeClass(done) {
  return "badge " + (done ? "badge-success" : "badge-neutral") + " text-xs";
}

const toolMessageClass = computed(() =>
  toolMessageOk.value
    ? "text-[hsl(var(--muted-foreground))]"
    : "text-red-500",
);

function formatTime(ts) {
  return ts ? new Date(ts * 1000).toLocaleString() : t("never");
}
function formatBytes(bytes) {
  if (!bytes || bytes <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  let i = 0,
    v = bytes;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return v.toFixed(i > 0 ? 1 : 0) + " " + units[i];
}

// Human-readable download speed, e.g. "1.2 MB/s".
function formatSpeed(bps) {
  if (!bps || bps <= 0) return "0 B/s";
  const units = ["B/s", "KB/s", "MB/s", "GB/s"];
  let i = 0,
    v = bps;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return v.toFixed(i > 0 ? 1 : 0) + " " + units[i];
}

// Human-readable remaining time, e.g. "2 分 5 秒".
function formatEta(sec) {
  if (sec === undefined || sec < 0) return t("eta_unknown");
  if (sec === 0) return t("eta_done");
  if (sec < 60) return sec + " " + t("eta_sec");
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  if (m < 60) return m + " " + t("eta_min") + " " + s + " " + t("eta_sec");
  const h = Math.floor(m / 60);
  const mm = m % 60;
  return h + " " + t("eta_hour") + " " + mm + " " + t("eta_min");
}

async function safeFetch(fn, maxRetries = 3) {
  for (let i = 0; i < maxRetries; i++) {
    try {
      return await fn();
    } catch (e) {
      if (i === maxRetries - 1) throw e;
      await new Promise((r) => setTimeout(r, 500 * (i + 1)));
    }
  }
}

async function refreshStatus() {
  try {
    const d = await safeFetch(() => api.getStatus());
    Object.assign(status, d);
    retryCount = 0;
  } catch (e) {
    retryCount++;
    if (retryCount <= 5) setTimeout(refreshStatus, 1000);
  }
}
async function refreshConfig() {
  try {
    Object.assign(settings, await safeFetch(() => api.getConfig()));
    parseKeyMappings(settings.key_mappings);
  } catch (e) {}
}
// Parse the newline-joined "src=dst" string into the editor rows.
function parseKeyMappings(str) {
  keyMappings.length = 0;
  if (!str) return;
  String(str)
    .split("\n")
    .forEach((line) => {
      const x = line.trim();
      if (!x) return;
      const i = x.indexOf("=");
      if (i <= 0) return;
      keyMappings.push({ src: x.slice(0, i).trim(), dst: x.slice(i + 1).trim() });
    });
}
// Serialize editor rows back into the "src=dst" newline string.
function serializeKeyMappings() {
  return keyMappings
    .filter((m) => m.src && m.dst)
    .map((m) => m.src + "=" + m.dst)
    .join("\n");
}
async function saveSettings(forceRecheck = false, showToastFlag = false) {
  try {
    settings.key_mappings = serializeKeyMappings();
    await api.updateConfig(settings);
    await refreshConfig();
    await refreshStatus();
    // When channel or download_source changes, reset state and re-check
    if (forceRecheck) {
      await api.resetUpdate();
      await refreshStatus();
      await checkUpdate();
    }
    if (showToastFlag) showToast(t("save_success"), "success");
  } catch (e) {
    if (showToastFlag) showToast(t("save_failed"), "error");
  }
}
function toggle(key) {
  const next = !settings[key];
  settings[key] = next;
  saveSettings();
  // When the user enables auto-check, run a check immediately so they get
  // instant feedback instead of having to reopen the page.
  if (key === "auto_check" && next) {
    checkAllUpdates();
  }
}
function addMapping() {
  keyMappings.push({ src: "", dst: "" });
}
function removeMapping(idx) {
  keyMappings.splice(idx, 1);
  saveSettings();
}

async function checkUpdate() {
  hasChecked.value = true;
  checking.value = true;
  try {
    await api.checkForUpdates();
    await waitForState((s) => s !== "checking", 30000);
  } catch (e) {
  } finally {
    checking.value = false;
    await refreshStatus();
  }
}
async function startDownload() {
  downloading.value = true;
  try {
    await api.download();
  } catch (e) {
  } finally {
    downloading.value = false;
    await refreshStatus();
  }
}
async function offlineInstall() {
  downloading.value = true;
  try {
    await api.installOffline();
    // Wait for state to leave idle (extraction starts immediately)
    await waitForState((s) => s !== "idle", 30000);
  } catch (e) {
  } finally {
    downloading.value = false;
    await refreshStatus();
  }
}
async function cancelDownload() {
  try {
    await api.cancel();
    await refreshStatus();
  } catch (e) {}
}
async function resetUpdate() {
  try {
    await api.resetUpdate();
    await refreshStatus();
  } catch (e) {}
}
async function applyAndRestart() {
  try {
    await api.restart();
  } catch (e) {}
}

// Self-update (Release channel only)
async function checkSelfUpdate() {
  hasChecked.value = true;
  selfChecking.value = true;
  try {
    const data = await api.checkSelfUpdate();
    Object.assign(status, data);
    await refreshStatus();
  } catch (e) {
  } finally {
    selfChecking.value = false;
  }
}

// Manual "Check for Updates": runs the Chrome update check and the ChromeGreen
// self-update check together. They are independent; a failure or a slower check
// on one side does not block the other.
function checkAllUpdates() {
  checkUpdate();
  checkSelfUpdate();
}
async function downloadSelfUpdate() {
  try {
    await api.downloadSelfUpdate();
    await refreshStatus();
  } catch (e) {
  }
}
async function applySelfUpdate() {
  try {
    await api.applySelfUpdate();
  } catch (e) {}
}

function applyTheme(m) {
  if (m !== "auto")
    document.documentElement.classList.toggle("dark", m === "dark");
  else
    document.documentElement.classList.toggle(
      "dark",
      window.matchMedia("(prefers-color-scheme: dark)").matches,
    );
}
function setThemeMode(m) {
  themeMode.value = m;
  showThemeDropdown.value = false;
  applyTheme(m);
  localStorage.setItem("chrome_green_theme", m);
  settings.theme = m;
  saveSettings();
}
function setLangMode(m) {
  langMode.value = m;
  showLangDropdown.value = false;
  setLocale(m);
  settings.language = m;
  saveSettings();
}
function toggleThemeDropdown() {
  showThemeDropdown.value = !showThemeDropdown.value;
  showLangDropdown.value = false;
}
function toggleLangDropdown() {
  showLangDropdown.value = !showLangDropdown.value;
  showThemeDropdown.value = false;
}

function waitForState(predicate, timeout) {
  return new Promise((resolve, reject) => {
    const start = Date.now();
    const check = async () => {
      if (Date.now() - start > timeout) {
        reject(new Error("Timeout"));
        return;
      }
      try {
        const data = await api.getStatus();
        Object.assign(status, data);
        if (predicate(data.state)) resolve();
        else setTimeout(check, 500);
      } catch (e) {
        setTimeout(check, 1000);
      }
    };
    check();
  });
}

function handleThemeChange(e) {
  if (themeMode.value === "auto")
    document.documentElement.classList.toggle("dark", e.matches);
}
function handleClickOutside(e) {
  // Close dropdowns when clicking outside
  if (
    !e.target.closest(".dropdown-menu") &&
    !e.target.closest(".icon-btn")
  ) {
    showThemeDropdown.value = false;
    showLangDropdown.value = false;
  }
}

// Debug log
async function loadLogs() {
  try {
    const d = await api.getLogs();
    if (d && d.logs) logs.value = d.logs;
  } catch (e) {}
}
async function copyLogs() {
  try {
    await navigator.clipboard.writeText(logs.value.join("\n"));
    logCopied.value = true;
    setTimeout(() => (logCopied.value = false), 2000);
  } catch (e) {}
}
async function clearLogs() {
  try {
    await api.clearLogs();
    logs.value = [];
  } catch (e) {}
}

// --- Tools ---
async function refreshToolsStatus() {
  try {
    const d = await safeFetch(() => api.getToolsStatus());
    if (d) {
      tools.desktop_shortcut = !!d.desktop_shortcut;
    }
  } catch (e) {}
}

async function createShortcut() {
  shortcutBusy.value = true;
  toolMessage.value = "";
  try {
    const d = await api.createDesktopShortcut();
    tools.desktop_shortcut = !!d.done;
    toolMessageOk.value = !!d.success;
    toolMessage.value = d.success
      ? d.done
        ? t("tool_ok_shortcut")
        : t("tool_fail_generic")
      : d.message || t("tool_fail_generic");
  } catch (e) {
    toolMessageOk.value = false;
    toolMessage.value = t("tool_fail_generic");
  } finally {
    shortcutBusy.value = false;
  }
}

// --- Chrome default data directory cleaner ---
async function refreshChromeDataStatus() {
  try {
    const d = await safeFetch(() => api.getChromeDataStatus());
    if (d) {
      chromeData.exists = !!d.exists;
      chromeData.empty = !!d.empty;
      chromeData.entry_count = d.entry_count || 0;
    }
  } catch (e) {}
}

async function cleanChromeData() {
  chromeDataBusy.value = true;
  chromeDataMessage.value = "";
  try {
    const d = await api.cleanChromeData();
    chromeDataMessageOk.value = !!d.success;
    chromeDataMessage.value = d.success
      ? t("tool_ok_clean")
      : d.message || t("tool_fail_generic");
    if (d.success) {
      // Directory is gone -> nothing left to clean.
      chromeData.exists = false;
      chromeData.empty = true;
      chromeData.entry_count = 0;
    }
  } catch (e) {
    chromeDataMessageOk.value = false;
    chromeDataMessage.value = t("tool_fail_generic");
  } finally {
    chromeDataBusy.value = false;
  }
}

// One-time app initialization (called from App.vue onMounted).
async function initApp() {
  const mq = window.matchMedia("(prefers-color-scheme: dark)");
  mq.addEventListener("change", handleThemeChange);
  document.addEventListener("click", handleClickOutside);

  // Apply the persisted theme immediately (before any network fetch) so the
  // page paints in the correct mode on reload. The inline <head> script in
  // index.html already set <html class="dark"> for the same purpose; this
  // keeps the Vue root in sync. The backend value is reconciled below.
  const initialTheme = localStorage.getItem("chrome_green_theme") || "auto";
  themeMode.value = initialTheme;
  applyTheme(initialTheme);

  await refreshStatus();
  await refreshConfig();

  // Backend INI is the source of truth for appearance; fall back to
  // this session's localStorage, then to "auto".
  const savedTheme =
    settings.theme ?? localStorage.getItem("chrome_green_theme") ?? "auto";
  const savedLang =
    settings.language ?? localStorage.getItem("chrome_green_lang") ?? "auto";
  themeMode.value = savedTheme;
  langMode.value = savedLang;
  settings.theme = savedTheme;
  settings.language = savedLang;
  applyTheme(savedTheme);
  setLocale(savedLang);

  updatePolling();
  startStatusStream();

  // Auto-check on page open only when the user enabled it; otherwise the
  // status stays "未检查" until they click "检查更新". Skip when a download
  // is in progress or an update is already staged (don't clobber those), but
  // still re-check for idle/error/available states, since a stale "available"
  // state must not suppress a configured auto-check.
  if (
    settings.auto_check &&
    status.state !== "downloading" &&
    status.state !== "ready" &&
    status.state !== "pending_apply" &&
    !isAutoChecking.value &&
    !selfChecking.value
  ) {
    checkAllUpdates();
  }

  await refreshChromeDataStatus();
}

function disposeApp() {
  stopStatusStream();
  if (logPollTimer) clearInterval(logPollTimer);
  if (toolPollTimer) clearInterval(toolPollTimer);
  const mq = window.matchMedia("(prefers-color-scheme: dark)");
  mq.removeEventListener("change", handleThemeChange);
  document.removeEventListener("click", handleClickOutside);
}

export function useStore() {
  return {
    // state
    status,
    settings,
    keyMappings,
    checking,
    downloading,
    selfChecking,
    selfDownloading,
    hasChecked,
    themeMode,
    langMode,
    logs,
    logCopied,
    showThemeDropdown,
    showLangDropdown,
    activeTab,
    tabs,
    tools,
    shortcutBusy,
    toolMessage,
    toolMessageOk,
    chromeData,
    chromeDataBusy,
    chromeDataMessage,
    chromeDataMessageOk,
    toast,
    // computeds
    channelOptions,
    checkIntervalOptions,
    proxyTypeOptions,
    version,
    githubUrl,
    themeClass,
    selfHasUpdate,
    isAutoChecking,
    etaText,
    statusLabel,
    statusBadgeClass,
    toolBadgeClass,
    toolMessageClass,
    // helpers
    t,
    formatTime,
    formatBytes,
    formatSpeed,
    formatEta,
    showToast,
    // actions
    saveSettings,
    toggle,
    addMapping,
    removeMapping,
    parseKeyMappings,
    serializeKeyMappings,
    checkUpdate,
    checkAllUpdates,
    startDownload,
    offlineInstall,
    cancelDownload,
    resetUpdate,
    applyAndRestart,
    checkSelfUpdate,
    downloadSelfUpdate,
    applySelfUpdate,
    setThemeMode,
    setLangMode,
    toggleThemeDropdown,
    toggleLangDropdown,
    loadLogs,
    copyLogs,
    clearLogs,
    refreshToolsStatus,
    createShortcut,
    refreshChromeDataStatus,
    cleanChromeData,
    refreshStatus,
    refreshConfig,
    startStatusStream,
    stopStatusStream,
    setKeyCaptureCallback,
    updatePolling,
    handleThemeChange,
    handleClickOutside,
    initApp,
    disposeApp,
  };
}
