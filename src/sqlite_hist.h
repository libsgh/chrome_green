// Minimal read-only SQLite 3 parser for Chrome's History database.
//
// Goal: extract the most recently visited URLs (recently opened tabs) so the
// jump list can show a "Recent" category, WITHOUT linking the full SQLite
// library (keeps version.dll small — the user is size-sensitive).
//
// Scope (intentionally tiny):
//   * Opens the file memory-mapped (no full read into the heap).
//   * Walks table b-tree pages (leaf 0x0D / interior 0x05) and decodes records.
//   * Handles record payload overflow chains (so large rows don't corrupt).
//   * NO indexes, NO joins, NO writes, NO transactions.
// Best-effort: a torn page (if Chrome writes mid-read) may yield a skipped row,
// never a crash (all reads are bounds-checked).

#pragma once

#include <string>
#include <vector>

namespace sqlite_hist {

struct Entry {
  std::wstring url;
  std::wstring title;
};

// Reads Chrome's Favicons SQLite DB and returns the PNG image data for the
// favicon associated with `page_url`.  Returns false on any error or if no
// favicon is found.  The PNG bytes are written to `out` (may be up to tens of
// KB — favicons are small).
bool ReadFaviconPng(const std::wstring& favicons_path,
                    const std::wstring& page_url,
                    std::vector<uint8_t>& out);

// Reads Chrome's History SQLite file at `history_path`, returns up to `limit`
// most-recently-visited entries (by last_visit_time descending) whose URL is a
// web URL (http/https). Returns false on any error (missing/locked/not a db).
bool ReadRecentUrls(const std::wstring& history_path, int limit,
                    std::vector<Entry>& out);

}  // namespace sqlite_hist
