// Minimal read-only SQLite 3 reader — see sqlite_hist.h for scope/limits.
#include "sqlite_hist.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace sqlite_hist {

namespace {

// --- memory-mapped database handle -------------------------------------------
struct Db {
  HANDLE hFile = INVALID_HANDLE_VALUE;
  HANDLE hMap = nullptr;
  const uint8_t* base = nullptr;
  size_t size = 0;
  uint32_t pageSize = 0;
  uint32_t usable = 0;  // page size minus reserved space at end of page

  bool Open(const wchar_t* path) {
    hFile = CreateFileW(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hFile, &sz)) {
      CloseHandle(hFile);
      hFile = INVALID_HANDLE_VALUE;
      return false;
    }
    size = static_cast<size_t>(sz.QuadPart);
    if (size < 112) {  // file header is 100 bytes; need at least one page
      CloseHandle(hFile);
      hFile = INVALID_HANDLE_VALUE;
      return false;
    }
    hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
      CloseHandle(hFile);
      hFile = INVALID_HANDLE_VALUE;
      return false;
    }
    base = static_cast<const uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
    if (!base) {
      CloseHandle(hMap);
      CloseHandle(hFile);
      hMap = nullptr;
      hFile = INVALID_HANDLE_VALUE;
      return false;
    }
    if (std::memcmp(base, "SQLite format 3", 15) != 0) return false;
    uint32_t ps = (uint32_t(base[16]) << 8) | base[17];
    pageSize = (ps == 1) ? 65536u : ps;
    uint8_t reserved = base[20];
    usable = pageSize - reserved;
    return true;
  }

  ~Db() {
    if (base) UnmapViewOfFile(base);
    if (hMap) CloseHandle(hMap);
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
  }

  // Start of page `pgno` (1-based) in the file. Page 1 begins at offset 0.
  const uint8_t* PageBase(uint32_t pgno) const {
    if (pgno < 1) return nullptr;
    if (pgno == 1) return base;
    size_t off = static_cast<size_t>(pgno - 1) * pageSize;
    if (off + pageSize > size) return nullptr;
    return base + off;
  }
};

// --- varint (SQLite serializes rowid / header lengths as 1..9-byte varints) --
static bool ReadVarint(const uint8_t* p, size_t avail, uint64_t& out, size_t& n) {
  out = 0;
  n = 0;
  for (int i = 0; i < 9; ++i) {
    if (static_cast<size_t>(i) >= avail) return false;
    uint8_t b = p[i];
    if (i == 8) {
      out = (out << 8) | b;
      n = 9;
      return true;
    }
    out = (out << 7) | (b & 0x7F);
    n = static_cast<size_t>(i + 1);
    if (!(b & 0x80)) return true;
  }
  return false;
}

// Big-endian signed integer of `n` bytes (n <= 8).
static int64_t ReadSigned(const uint8_t* p, int n) {
  int64_t v = 0;
  for (int k = 0; k < n; ++k) v = (v << 8) | p[k];
  int bits = n * 8;
  if (bits < 64) {
    int64_t sign = (int64_t)1 << (bits - 1);
    if (v & sign) v -= ((int64_t)1 << bits);
  }
  return v;
}

static uint32_t ReadU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// One decoded column value.
struct Val {
  bool isNull = false;
  int64_t i = 0;
  std::wstring s;
  std::vector<uint8_t> blob;  // populated for BLOB serial types
};

// Number of local bytes stored for a record of payload length P on a table
// leaf page (SQLite overflow algorithm). Kept on 32-bit math (P is tiny here).
static uint32_t LocalPayload(uint32_t P, uint32_t usable) {
  uint32_t X = usable - 35;  // max local for table leaf
  if (P <= X) return P;
  uint32_t minLocal = ((usable - 4) * 32 / 255) - 23;
  uint32_t K = minLocal + (P - minLocal) % (usable - 4);
  if (K > X) K = minLocal + (P - minLocal - 1) % (usable - 4);
  return K;
}

// Decode a record (payload bytes) into columns.
static void DecodeRecord(const std::vector<uint8_t>& rec, std::vector<Val>& cols) {
  cols.clear();
  uint64_t hlen;
  size_t hn;
  if (!ReadVarint(rec.data(), rec.size(), hlen, hn)) return;
  size_t p = hn;
  std::vector<uint64_t> stypes;
  while (p < hn + static_cast<size_t>(hlen) && p < rec.size()) {
    uint64_t st;
    size_t sn;
    if (!ReadVarint(rec.data() + p, rec.size() - p, st, sn)) break;
    stypes.push_back(st);
    p += sn;
  }
  size_t body = static_cast<size_t>(hlen);
  for (size_t i = 0; i < stypes.size(); ++i) {
    uint64_t t = stypes[i];
    Val v;
    if (t == 0) {
      v.isNull = true;
    } else if (t >= 1 && t <= 4) {
      int n = static_cast<int>(t);
      v.i = ReadSigned(rec.data() + body, n);
      body += n;
    } else if (t == 5) {
      v.i = ReadSigned(rec.data() + body, 6);
      body += 6;
    } else if (t == 6 || t == 7) {
      v.i = ReadSigned(rec.data() + body, 8);
      body += 8;
    } else if (t == 8) {
      v.i = 0;
    } else if (t == 9) {
      v.i = 1;
    } else {
      size_t len = (t >= 12) ? static_cast<size_t>((t - 12) / 2)
                             : static_cast<size_t>((t - 13) / 2);
      if (t & 1) {
        if (body + len <= rec.size()) {
          int wlen = MultiByteToWideChar(
              CP_UTF8, 0, reinterpret_cast<const char*>(rec.data() + body),
              static_cast<int>(len), nullptr, 0);
          if (wlen > 0) {
            v.s.resize(static_cast<size_t>(wlen));
            MultiByteToWideChar(
                CP_UTF8, 0, reinterpret_cast<const char*>(rec.data() + body),
                static_cast<int>(len), &v.s[0], wlen);
          }
        }
      } else {
        // BLOB — capture the raw bytes.
        if (body + len <= rec.size()) {
          v.blob.assign(rec.data() + body, rec.data() + body + len);
        }
      }
      body += len;
    }
    cols.push_back(v);
  }
}

// Parse "CREATE TABLE urls(...)" and record the index of url/title/
// last_visit_time columns. Case-insensitive, handles quoted identifiers.
static bool CiEq(const std::wstring& a, const wchar_t* b) {
  if (a.size() != std::wcslen(b)) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::towlower(a[i]) != std::towlower(b[i])) return false;
  return true;
}

static void ParseColumnIndices(const std::wstring& sql, int& idxUrl,
                               int& idxTitle, int& idxLvt) {
  size_t start = sql.find(L'(');
  if (start == std::wstring::npos) return;
  int idx = 0;
  int depth = 0;
  std::wstring cur;
  for (size_t i = start + 1; i < sql.size(); ++i) {
    wchar_t c = sql[i];
    if (c == L'(') {
      depth++;
      cur += c;
    } else if (c == L')') {
      if (depth == 0) break;
      depth--;
      cur += c;
    } else if (c == L',' && depth == 0) {
      // first token of `cur` is the column name
      size_t a = cur.find_first_not_of(L" \t\r\n");
      if (a != std::wstring::npos) {
        size_t b = cur.find_first_of(L" \t\r\n(\"", a);
        std::wstring name = cur.substr(a, b == std::wstring::npos
                                              ? std::wstring::npos
                                              : b - a);
        if (name.size() >= 2 &&
            (name.front() == L'"' || name.front() == L'`'))
          name = name.substr(1, name.size() - 2);
        if (CiEq(name, L"url")) idxUrl = idx;
        else if (CiEq(name, L"title")) idxTitle = idx;
        else if (CiEq(name, L"last_visit_time")) idxLvt = idx;
      }
      cur.clear();
      idx++;
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    size_t a = cur.find_first_not_of(L" \t\r\n");
    if (a != std::wstring::npos) {
      size_t b = cur.find_first_of(L" \t\r\n(\"", a);
      std::wstring name = cur.substr(a, b == std::wstring::npos
                                            ? std::wstring::npos
                                            : b - a);
      if (name.size() >= 2 && (name.front() == L'"' || name.front() == L'`'))
        name = name.substr(1, name.size() - 2);
      if (CiEq(name, L"url")) idxUrl = idx;
      else if (CiEq(name, L"title")) idxTitle = idx;
      else if (CiEq(name, L"last_visit_time")) idxLvt = idx;
    }
  }
}

template <typename F>
static void WalkTable(const Db& db, uint32_t pgno, F&& cb) {
  const uint8_t* pb = db.PageBase(pgno);
  if (!pb) return;
  uint32_t hdrOff = (pgno == 1) ? 100u : 0u;
  const uint8_t* hdr = pb + hdrOff;
  uint8_t type = hdr[0];
  uint16_t ncells = (uint16_t(hdr[3]) << 8) | hdr[4];
  uint32_t cellPtrStart = hdrOff + (type == 0x05 ? 12u : 8u);
  const uint8_t* pageEnd = pb + db.pageSize;

  if (type == 0x05) {  // interior table page
    for (uint16_t i = 0; i < ncells; ++i) {
      uint16_t cp =
          (uint16_t(pb[cellPtrStart + i * 2]) << 8) | pb[cellPtrStart + i * 2 + 1];
      uint32_t child = ReadU32(pb + cp);
      WalkTable(db, child, cb);
    }
    uint32_t right = ReadU32(hdr + 8);
    if (right) WalkTable(db, right, cb);
  } else if (type == 0x0D) {  // leaf table page
    for (uint16_t i = 0; i < ncells; ++i) {
      uint16_t cp =
          (uint16_t(pb[cellPtrStart + i * 2]) << 8) | pb[cellPtrStart + i * 2 + 1];
      const uint8_t* cell = pb + cp;
      const uint8_t* pageEndC = pageEnd;
      uint64_t P;
      size_t pn;
      if (!ReadVarint(cell, pageEndC - cell, P, pn)) continue;
      uint64_t rowid;
      size_t rn;
      if (!ReadVarint(cell + pn, pageEndC - (cell + pn), rowid, rn)) continue;
      const uint8_t* data = cell + pn + rn;

      uint32_t P32 = (P > 0xFFFFFFFFu) ? 0xFFFFFFFFu : static_cast<uint32_t>(P);
      uint32_t K = LocalPayload(P32, db.usable);
      uint32_t remaining = static_cast<uint32_t>(pageEndC - data);
      if (K > remaining) K = remaining;

      std::vector<uint8_t> rec;
      rec.reserve(P32);
      uint32_t have = 0;
      for (uint32_t j = 0; j < K && data + j < pageEndC; ++j)
        rec.push_back(data[j]);
      have = static_cast<uint32_t>(rec.size());

      if (P32 > K && (data + K + 4) <= pageEndC) {
        uint32_t ovfl = ReadU32(data + K);
        while (have < P32 && ovfl) {
          const uint8_t* op = db.PageBase(ovfl);
          if (!op) break;
          uint32_t next = ReadU32(op);
          const uint8_t* chunk = op + 4;
          uint32_t cap = db.usable - 4;
          uint32_t need = P32 - have;
          uint32_t take = (cap < need) ? cap : need;
          for (uint32_t j = 0; j < take; ++j) rec.push_back(chunk[j]);
          have += take;
          ovfl = next;
        }
      }

      std::vector<Val> cols;
      DecodeRecord(rec, cols);
      if (!cols.empty()) cb(cols);
    }
  }
  // index pages (0x02/0x0A) and other types: ignore.
}

struct Row {
  int64_t lvt;
  std::wstring url;
  std::wstring title;
};

}  // namespace

bool ReadRecentUrls(const std::wstring& history_path, int limit,
                    std::vector<Entry>& out) {
  out.clear();
  if (limit <= 0) return false;
  Db db;
  if (!db.Open(history_path.c_str())) return false;

  // 1) Find the `urls` table: its root page and column indices.
  uint32_t urlsRoot = 0;
  int idxUrl = -1, idxTitle = -1, idxLvt = -1;
  WalkTable(db, 1, [&](const std::vector<Val>& cols) {
    if (cols.size() < 5) return;
    if (!CiEq(cols[0].s, L"table")) return;
    if (!CiEq(cols[1].s, L"urls")) return;
    urlsRoot = static_cast<uint32_t>(cols[3].i);
    ParseColumnIndices(cols[4].s, idxUrl, idxTitle, idxLvt);
  });
  if (!urlsRoot) return false;
  if (idxUrl < 0) idxUrl = 1;
  if (idxTitle < 0) idxTitle = 2;
  if (idxLvt < 0) idxLvt = 5;

  // 2) Walk `urls`, keep the top-`limit` by last_visit_time.
  std::vector<Row> top(limit);
  int topn = 0;
  WalkTable(db, urlsRoot, [&](const std::vector<Val>& cols) {
    if (static_cast<int>(cols.size()) <= idxUrl) return;
    const Val& u = cols[idxUrl];
    if (u.isNull || u.s.empty()) return;
    // Only web tabs; skip internal/about/file URLs.
    if (u.s.compare(0, 9, L"chrome://") == 0 ||
        u.s.compare(0, 6, L"about:") == 0 ||
        u.s.compare(0, 7, L"file://") == 0)
      return;
    int64_t lvt = (idxLvt < static_cast<int>(cols.size()) && !cols[idxLvt].isNull)
                     ? cols[idxLvt].i
                     : 0;
    if (lvt == 0) return;
    std::wstring title =
        (idxTitle < static_cast<int>(cols.size()) && !cols[idxTitle].isNull &&
         !cols[idxTitle].s.empty())
            ? cols[idxTitle].s
            : u.s;

    if (topn < limit) {
      top[topn].lvt = lvt;
      top[topn].url = u.s;
      top[topn].title = title;
      topn++;
    } else {
      int mi = 0;
      for (int k = 1; k < topn; ++k)
        if (top[k].lvt < top[mi].lvt) mi = k;
      if (lvt > top[mi].lvt) {
        top[mi].lvt = lvt;
        top[mi].url = u.s;
        top[mi].title = title;
      }
    }
  });

  std::sort(top.begin(), top.begin() + topn,
            [](const Row& a, const Row& b) { return a.lvt > b.lvt; });
  for (int i = 0; i < topn; ++i)
    out.push_back({top[i].url, top[i].title});
  return true;
}

// --- Favicon reader (Chrome's Favicons DB) -----------------------------------

// Helper: find the column index for a given name in a CREATE TABLE sql string.
static int FindColumnIndex(const std::wstring& sql, const wchar_t* name) {
  size_t start = sql.find(L'(');
  if (start == std::wstring::npos) return -1;
  int idx = 0;
  int depth = 0;
  std::wstring cur;
  for (size_t i = start + 1; i < sql.size(); ++i) {
    wchar_t c = sql[i];
    if (c == L'(') { depth++; cur += c; }
    else if (c == L')') {
      if (depth == 0) break;
      depth--; cur += c;
    } else if (c == L',' && depth == 0) {
      size_t a = cur.find_first_not_of(L" \t\r\n");
      if (a != std::wstring::npos) {
        size_t b = cur.find_first_of(L" \t\r\n(\"", a);
        std::wstring col = cur.substr(a, b == std::wstring::npos
                                           ? std::wstring::npos : b - a);
        if (col.size() >= 2 &&
            (col.front() == L'"' || col.front() == L'`'))
          col = col.substr(1, col.size() - 2);
        if (CiEq(col, name)) return idx;
      }
      cur.clear();
      idx++;
    } else {
      cur += c;
    }
  }
  // last column (no trailing comma)
  if (!cur.empty()) {
    size_t a = cur.find_first_not_of(L" \t\r\n");
    if (a != std::wstring::npos) {
      size_t b = cur.find_first_of(L" \t\r\n(\"", a);
      std::wstring col = cur.substr(a, b == std::wstring::npos
                                         ? std::wstring::npos : b - a);
      if (col.size() >= 2 && (col.front() == L'"' || col.front() == L'`'))
        col = col.substr(1, col.size() - 2);
      if (CiEq(col, name)) return idx;
    }
  }
  return -1;
}

// Normalize a URL for fuzzy matching against icon_mapping.page_url:
// strip fragment, lowercase the scheme, drop a single trailing slash.
static std::wstring NormalizeUrl(const std::wstring& u) {
  std::wstring s = u;
  size_t h = s.find(L'#');
  if (h != std::wstring::npos) s = s.substr(0, h);
  size_t colon = s.find(L':');
  if (colon != std::wstring::npos) {
    for (size_t i = 0; i < colon; ++i)
      s[i] = static_cast<wchar_t>(std::towlower(s[i]));
  }
  if (s.size() > 8 && s.back() == L'/') s.pop_back();
  return s;
}

// Extract the host portion of a URL (used as a last-resort fuzzy match).
static std::wstring GetHost(const std::wstring& u) {
  size_t d = u.find(L"://");
  size_t start = (d == std::wstring::npos) ? 0 : d + 3;
  size_t end = u.find(L'/', start);
  if (end == std::wstring::npos) return u.substr(start);
  return u.substr(start, end - start);
}

bool ReadFaviconPng(const std::wstring& favicons_path,
                    const std::wstring& page_url,
                    std::vector<uint8_t>& out) {
  out.clear();
  Db db;
  if (!db.Open(favicons_path.c_str())) return false;

  // 1) Find icon_mapping table: get rootpage + column indices.
  uint32_t imRoot = 0;
  int imPageUrl = -1, imIconId = -1;
  WalkTable(db, 1, [&](const std::vector<Val>& cols) {
    if (cols.size() < 5) return;
    if (!CiEq(cols[0].s, L"table")) return;
    if (!CiEq(cols[1].s, L"icon_mapping")) return;
    imRoot = static_cast<uint32_t>(cols[3].i);
    imPageUrl = FindColumnIndex(cols[4].s, L"page_url");
    imIconId = FindColumnIndex(cols[4].s, L"icon_id");
  });
  if (!imRoot || imPageUrl < 0 || imIconId < 0) return false;

  // 2) Collect ALL icon_mapping rows (page_url -> icon_id) by value.
  //    We keep them so we can try several matching strategies without
  //    re-walking the DB.
  std::vector<std::pair<std::wstring, int64_t>> mappings;
  WalkTable(db, imRoot, [&](const std::vector<Val>& cols) {
    if (static_cast<int>(cols.size()) <= imPageUrl) return;
    const Val& u = cols[imPageUrl];
    if (u.isNull || u.s.empty()) return;
    int64_t id = (imIconId < static_cast<int>(cols.size()) && !cols[imIconId].isNull)
                     ? cols[imIconId].i : -1;
    if (id >= 0) mappings.emplace_back(u.s, id);
  });
  if (mappings.empty()) return false;

  // 3) Find favicon_bitmaps table: get rootpage + column indices.
  uint32_t fbRoot = 0;
  int fbIconId = -1, fbWidth = -1, fbImageData = -1;
  WalkTable(db, 1, [&](const std::vector<Val>& cols) {
    if (cols.size() < 5) return;
    if (!CiEq(cols[0].s, L"table")) return;
    if (!CiEq(cols[1].s, L"favicon_bitmaps")) return;
    fbRoot = static_cast<uint32_t>(cols[3].i);
    fbIconId = FindColumnIndex(cols[4].s, L"icon_id");
    fbWidth = FindColumnIndex(cols[4].s, L"width");
    fbImageData = FindColumnIndex(cols[4].s, L"image_data");
  });
  if (!fbRoot || fbIconId < 0 || fbWidth < 0 || fbImageData < 0) return false;

  // 4) Collect favicon_bitmaps by icon_id, keeping the best-size PNG per id.
  //    Stored BY VALUE (blob copied) so pointers stay valid after the walk.
  struct Bmp { int width = 0; std::vector<uint8_t> data; };
  std::unordered_map<int64_t, Bmp> bitmaps;
  WalkTable(db, fbRoot, [&](const std::vector<Val>& cols) {
    if (static_cast<int>(cols.size()) <= fbIconId) return;
    const Val& iid = cols[fbIconId];
    if (iid.isNull) return;
    if (fbWidth >= static_cast<int>(cols.size()) || cols[fbWidth].isNull)
      return;
    if (fbImageData >= static_cast<int>(cols.size()) ||
        cols[fbImageData].blob.empty())
      return;
    int64_t id = iid.i;
    int w = static_cast<int>(cols[fbWidth].i);
    int diff = (w >= 16) ? (w - 16) : (16 - w);
    auto it = bitmaps.find(id);
    bool better = false;
    if (it == bitmaps.end()) {
      better = true;
    } else {
      int bestDiff = (it->second.width >= 16) ? (it->second.width - 16)
                                               : (16 - it->second.width);
      if (diff < bestDiff) better = true;
      else if (diff == bestDiff && w > 16 && it->second.width < 16) better = true;
    }
    if (better) {
      Bmp b;
      b.width = w;
      b.data = cols[fbImageData].blob;  // copy by value
      bitmaps[id] = std::move(b);
    }
  });
  if (bitmaps.empty()) return false;

  // 5) Resolve our page_url to an icon_id using progressively looser matches.
  // 5a) exact
  int64_t targetIconId = -1;
  for (const auto& m : mappings)
    if (m.first == page_url) { targetIconId = m.second; break; }
  // 5b) normalized (fragment / trailing slash / scheme case)
  if (targetIconId < 0) {
    std::wstring nm = NormalizeUrl(page_url);
    for (const auto& m : mappings)
      if (NormalizeUrl(m.first) == nm) { targetIconId = m.second; break; }
  }
  // 5c) host-only (covers subpages / minor URL differences)
  if (targetIconId < 0) {
    std::wstring h = GetHost(page_url);
    for (const auto& m : mappings)
      if (GetHost(m.first) == h) { targetIconId = m.second; break; }
  }
  if (targetIconId < 0) return false;

  auto it = bitmaps.find(targetIconId);
  if (it == bitmaps.end() || it->second.data.empty()) return false;

  // 6) Return the PNG bytes (Chrome stores favicons as PNG in image_data).
  out = it->second.data;
  return true;
}

}  // namespace sqlite_hist
