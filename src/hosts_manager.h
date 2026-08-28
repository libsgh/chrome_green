#ifndef CHROME_GREEN_SRC_HOSTS_MANAGER_H_
#define CHROME_GREEN_SRC_HOSTS_MANAGER_H_

#include <string>
#include <vector>

// Domain-mapping (域名映射) subsystem.
//
// Instead of editing the system hosts file, this maps domains to IPs through
// Chromium's `--host-resolver-rules` switch, injected into the Chrome command
// line at launch (see portable.cc). It only affects this Chrome, needs no
// administrator rights, and takes effect after a restart.
//
// Subscription sources are plain hosts-format text (one "IP domain" per line).
// They are downloaded, parsed into (domain, ip) rules, cached under
// <AppDir>\..\Data\resolver_cache, and concatenated into the MAP string at
// launch. A hard cap (max_total) on the number of effective rules guards the
// command-line length limit.
namespace resolver {

struct ResolvedRule {
  std::wstring domain;
  std::wstring ip;
  std::wstring source;  // subscription name (for display)
};

// Parse a hosts-format text (UTF-8) into rules. Invalid lines are skipped and
// counted in `invalid_lines`. Returns true if any rule was produced.
bool ParseHostsText(const std::string& text, std::vector<ResolvedRule>& out,
                    int& invalid_lines);

// Build the Chromium `--host-resolver-rules` value body: "MAP d ip,MAP ...".
std::string RulesToMapBody(const std::vector<ResolvedRule>& rules);

// Build a hosts-format text ("IP domain" per line) for export / caching.
std::string RulesToHostsText(const std::vector<ResolvedRule>& rules);

// Full switch string `--host-resolver-rules="..."` built from the caches of
// all enabled subscriptions, or an empty string when disabled or no rules.
// Consumed by portable.cc's command-line injection.
std::wstring BuildResolverRulesSwitch();

// Aggregate effective rules from all enabled subscriptions (for display).
std::vector<ResolvedRule> GetEffectiveRules();

// Cached rules of a single subscription by 0-based index (for the expandable
// panels in the config page). Empty if the subscription has no cache or the
// index is out of range.
std::vector<ResolvedRule> GetSubscriptionRules(int index);

// Total rule count across enabled subscriptions.
int GetTotalRuleCount();

// HTTP GET the URL body as UTF-8. Returns false with `error` on failure.
bool HttpGetText(const std::string& url, std::string& out, std::wstring& error);

// Cache directory: <AppDir>\..\Data\resolver_cache (created if missing).
std::wstring GetResolverCacheDir();

// Add a subscription: download + parse, then enforce the total cap. On success
// writes the ini entry + cache and returns true; on failure sets `error` and
// returns false (the subscription is NOT added).
bool AddSubscription(const std::wstring& name, const std::wstring& url,
                     std::wstring& error);

// Refresh an existing subscription by 0-based index. If the refreshed count
// would push the enabled total over the cap, the old cache is kept and `error`
// is set (returns false). Otherwise updates the cache + rule_count +
// last_refresh.
bool RefreshSubscription(int index, std::wstring& error);

// Remove a subscription by 0-based index (deletes ini keys + cache file).
bool RemoveSubscription(int index);

// Enable/disable a subscription (writes ini + reloads).
bool SetSubscriptionEnabled(int index, bool enabled);

// Set the global refresh interval in hours (0 = manual). Writes ini.
void SetRefreshInterval(int hours);

// Export the effective (enabled) rules as a hosts-format file. `path_utf8` is
// an absolute UTF-8 path; when empty, uses
// <AppDir>\..\Data\resolver_export\hosts.<timestamp>.txt.
bool ExportRules(const std::string& path_utf8, std::wstring& error);

// Start the background periodic-refresh thread. No-op when the interval is 0
// or resolver is disabled. Safe to call once at startup.
void StartResolverRefresh();

}  // namespace resolver

#endif  // CHROME_GREEN_SRC_HOSTS_MANAGER_H_
