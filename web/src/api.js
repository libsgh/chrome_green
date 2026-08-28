// API client for ChromeGreen local HTTP server.
//
// BASE is ALWAYS empty (relative URLs). The config page is served by the
// same C++ HTTP server that handles /api/*, so relative requests resolve to
// the correct per-install port. That matters because each portable Chrome
// binds its own port (derived from the install path); a hardcoded host:port
// would send a page's requests to the wrong instance's server.
//
// In development (npm run dev), vite's dev server proxies relative /api
// requests to the running C++ server (see vite.config.js).

const BASE = ''

async function request(path, options = {}) {
  const url = `${BASE}${path}`
  const res = await fetch(url, options)
  if (!res.ok) {
    throw new Error(`HTTP ${res.status}: ${res.statusText}`)
  }
  return res.json()
}

export const api = {
  // Get current update status
  getStatus() {
    return request('/api/status')
  },

  // Get current configuration
  getConfig() {
    return request('/api/config')
  },

  // Update configuration
  updateConfig(config) {
    return request('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(config),
    })
  },

  // Trigger an update check
  checkForUpdates() {
    return request('/api/check', { method: 'POST' })
  },

  // Trigger download of found update
  download() {
    return request('/api/download', { method: 'POST' })
  },

  // Offline install: use a .7z package already in the updates/ folder
  installOffline() {
    return request('/api/install-offline', { method: 'POST' })
  },

  // Cancel ongoing download
  cancel() {
    return request('/api/cancel', { method: 'POST' })
  },

  // Reset update state to idle
  resetUpdate() {
    return request('/api/reset', { method: 'POST' })
  },

  // Mark update as ready for next restart
  apply() {
    return request('/api/apply', { method: 'POST' })
  },

  // Restart Chrome (applies pending update)
  restart() {
    return request('/api/restart', { method: 'POST' })
  },

  // Self-update: check for chrome_green updates from GitHub (Release channel)
  checkSelfUpdate() {
    return request('/api/self-update/check')
  },

  // Self-update: download new version.dll
  downloadSelfUpdate() {
    return request('/api/self-update/download', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({}),
    })
  },

  // Self-update: apply and restart
  applySelfUpdate() {
    return request('/api/self-update/apply', { method: 'POST' })
  },

  // Get debug logs
  getLogs() {
    return request('/api/logs')
  },

  // Clear debug logs
  clearLogs() {
    return request('/api/logs/clear', { method: 'POST' })
  },

  // Tools: get current tool state (shortcut created)
  getToolsStatus() {
    return request('/api/tools/status')
  },

  // Tools: create a desktop shortcut to the portable Chrome
  createDesktopShortcut() {
    return request('/api/tools/desktop-shortcut', { method: 'POST' })
  },

  // Tools: status of the default (non-portable) Chrome data directory
  getChromeDataStatus() {
    return request('/api/tools/chrome-data-status')
  },

  // Tools: clean the default Chrome data directory
  cleanChromeData() {
    return request('/api/tools/clean-chrome-data', { method: 'POST' })
  },

  // Hotkey capture: ask the backend to swallow Chrome shortcuts and report
  // the next key combination over the SSE 'key_capture' event.
  startCapture() {
    return request('/api/capture/start', { method: 'POST' })
  },

  // Abort an in-progress capture.
  stopCapture() {
    return request('/api/capture/stop', { method: 'POST' })
  },

  // Domain Mapping (域名映射): get config + subscriptions + effective rules.
  getResolver() {
    return request('/api/resolver')
  },

  // Set global resolver config (enabled / refresh_interval / max_total).
  updateResolverConfig(cfg) {
    return request('/api/resolver/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cfg),
    })
  },

  // Add a subscription (name + url).
  addSubscription(payload) {
    return request('/api/resolver/add', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  // Remove a subscription by index.
  removeSubscription(payload) {
    return request('/api/resolver/remove', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  // Enable/disable a subscription by index.
  setSubscriptionEnabled(payload) {
    return request('/api/resolver/enable', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  // Refresh a subscription (index>=0) or all enabled subscriptions (index=-1).
  refreshSubscription(payload) {
    return request('/api/resolver/refresh', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  // Export the effective rules as a hosts-format file.
  exportResolverRules() {
    return request('/api/resolver/export', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({}),
    })
  },
}
