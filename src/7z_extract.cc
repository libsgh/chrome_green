#include "7z_extract.h"

#include <vector>

#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>

#include "update.h"  // AddDebugLog

// LZMA SDK (git submodule: 7zip) — pure C, compiled as C++
extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
}

#pragma comment(lib, "shlwapi.lib")

namespace {

static void* SzAlloc(ISzAllocPtr, size_t s) { return malloc(s); }
static void SzFree(ISzAllocPtr, void* p) { free(p); }
static ISzAlloc g_Alloc = { SzAlloc, SzFree };

// 7z signature: {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}
static constexpr Byte k7zSignature[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};

// --- Seekable input stream backed by a Windows file handle ---
// Supports SFX archives by adding a base_offset: the stream behaves as if
// position 0 = start of the embedded 7z data inside the .exe.

typedef struct { ISeekInStream vt; HANDLE file; Int64 base_offset; } CInStream;

static SRes InStream_Read(const ISeekInStream* p, void* buf, size_t* size) {
  auto* s = Z7_CONTAINER_FROM_VTBL(p, CInStream, vt);
  DWORD read = 0;
  BOOL ok = ReadFile(s->file, buf, (DWORD)*size, &read, nullptr);
  *size = read;
  return ok ? SZ_OK : SZ_ERROR_READ;
}

static SRes InStream_Seek(const ISeekInStream* p, Int64* pos, ESzSeek origin) {
  auto* s = Z7_CONTAINER_FROM_VTBL(p, CInStream, vt);
  Int64 real_pos;
  DWORD method;

  switch (origin) {
    case SZ_SEEK_SET:
      real_pos = *pos + s->base_offset;
      method = FILE_BEGIN;
      break;
    case SZ_SEEK_CUR:
      real_pos = *pos;
      method = FILE_CURRENT;
      break;
    case SZ_SEEK_END:
      real_pos = *pos;
      method = FILE_END;
      break;
    default:
      return SZ_ERROR_READ;
  }

  LARGE_INTEGER li;
  li.QuadPart = real_pos;
  li.LowPart = SetFilePointer(s->file, li.LowPart, &li.HighPart, method);
  if (li.LowPart == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
    return SZ_ERROR_READ;

  // Report position relative to base_offset (virtual position)
  *pos = li.QuadPart - s->base_offset;
  return SZ_OK;
}

// Scan the file for the 7z signature. Returns the offset where the
// signature starts, or -1 if not found.
Int64 Find7zSignature(HANDLE file) {
  const DWORD kScanBufSize = 4096;
  std::vector<Byte> buf(kScanBufSize + 6);  // overlap for cross-block matches

  // First check position 0 (standalone .7z)
  LARGE_INTEGER zero = {{0, 0}};
  SetFilePointer(file, 0, &zero.HighPart, FILE_BEGIN);
  DWORD read = 0;
  if (ReadFile(file, buf.data(), 6, &read, nullptr) && read == 6) {
    if (memcmp(buf.data(), k7zSignature, 6) == 0) {
      AddDebugLog("7z signature found at position 0 (standalone archive)");
      return 0;
    }
  }

  // Scan for embedded 7z data in SFX (.exe) files
  // The 7z signature typically appears after the PE data.
  // Scan in overlapping blocks to handle cross-boundary matches.
  LARGE_INTEGER file_size_li;
  file_size_li.LowPart = GetFileSize(file, reinterpret_cast<LPDWORD>(&file_size_li.HighPart));
  Int64 file_size = file_size_li.QuadPart;

  AddDebugLog("Scanning for embedded 7z signature, file size: " +
              std::to_string(file_size) + " bytes");

  // Start scanning from position 1 (we already checked position 0)
  Int64 scan_pos = 1;
  while (scan_pos < file_size - 6) {
    // Seek to scan_pos and read a block
    LARGE_INTEGER li;
    li.QuadPart = scan_pos;
    SetFilePointer(file, li.LowPart, &li.HighPart, FILE_BEGIN);

    DWORD to_read = (DWORD)std::min<Int64>(kScanBufSize, file_size - scan_pos);
    if (to_read == 0) break;

    DWORD bytes_read = 0;
    if (!ReadFile(file, buf.data(), to_read, &bytes_read, nullptr) || bytes_read == 0)
      break;

    // Search for signature in this block
    for (DWORD i = 0; i < bytes_read - 5; i++) {
      if (memcmp(buf.data() + i, k7zSignature, 6) == 0) {
        Int64 found_offset = scan_pos + i;
        AddDebugLog("7z signature found at offset " + std::to_string(found_offset) +
                    " (SFX archive)");
        return found_offset;
      }
    }

    // Advance, but overlap by 5 bytes to catch cross-boundary matches
    scan_pos += bytes_read - 5;
    if (bytes_read < to_read) break;  // short read, probably EOF
  }

  AddDebugLog("No 7z signature found in file");
  return -1;
}

}  // namespace

bool Extract7zLegacy(const std::wstring& archive, const std::wstring& output_dir) {
  AddDebugLog("Extract7zLegacy: opening " + std::string(archive.begin(), archive.end()));

  // --- Open archive ---
  CInStream stream = {};
  stream.base_offset = 0;
  stream.file = CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (stream.file == INVALID_HANDLE_VALUE) {
    AddDebugLog("Extract7zLegacy: failed to open file, error " +
                std::to_string(GetLastError()));
    return false;
  }
  stream.vt.Read = InStream_Read;
  stream.vt.Seek = InStream_Seek;

  // --- Find 7z signature (supports SFX .exe archives) ---
  Int64 sig_offset = Find7zSignature(stream.file);
  if (sig_offset < 0) {
    AddDebugLog("Extract7zLegacy: no 7z signature found in file");
    CloseHandle(stream.file);
    return false;
  }
  stream.base_offset = sig_offset;

  // Seek to the start of 7z data (position 0 in virtual stream)
  LARGE_INTEGER li;
  li.QuadPart = sig_offset;
  SetFilePointer(stream.file, li.LowPart, &li.HighPart, FILE_BEGIN);

  CrcGenerateTable();

  // --- Parse 7z archive ---
  const size_t kLookBufSize = 1 << 18;  // 256 KB
  std::vector<Byte> look_buf(kLookBufSize);

  CLookToRead2 look;
  look.buf = look_buf.data();
  look.bufSize = look_buf.size();
  LookToRead2_CreateVTable(&look, False);
  look.realStream = &stream.vt;
  LookToRead2_INIT(&look);

  CSzArEx db;
  SzArEx_Init(&db);
  SRes res = SzArEx_Open(&db, &look.vt, &g_Alloc, &g_Alloc);
  if (res != SZ_OK) {
    AddDebugLog("Extract7zLegacy: SzArEx_Open failed, res=" + std::to_string(res));
    CloseHandle(stream.file);
    SzArEx_Free(&db, &g_Alloc);
    return false;
  }

  AddDebugLog("7z archive opened, " + std::to_string(db.NumFiles) + " files");

  // --- Ensure output directory exists ---
  SHCreateDirectoryExW(nullptr, output_dir.c_str(), nullptr);

  // --- Per-file extraction ---
  UInt32 block_index = 0xFFFFFFFF;
  Byte* out_buf = nullptr;
  size_t out_buf_size = 0;
  int extracted_count = 0;

  for (UInt32 i = 0; i < db.NumFiles; i++) {
    // Get file name (UTF-16)
    size_t name_len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
    if (name_len == 0 || name_len > 32768) continue;

    std::vector<UInt16> name_buf(name_len);
    SzArEx_GetFileNameUtf16(&db, i, name_buf.data());

    std::wstring rel_path((const wchar_t*)name_buf.data(), name_len - 1);  // exclude null terminator

    // Skip directories
    if (SzArEx_IsDir(&db, i)) {
      std::wstring dir_path = output_dir + L"\\" + rel_path;
      SHCreateDirectoryExW(nullptr, dir_path.c_str(), nullptr);
      continue;
    }

    // Extract file data
    size_t offset = 0;
    size_t out_size_processed = 0;
    res = SzArEx_Extract(&db, &look.vt, i, &block_index,
                         &out_buf, &out_buf_size,
                         &offset, &out_size_processed,
                         &g_Alloc, &g_Alloc);
    if (res != SZ_OK) {
      AddDebugLog("Extract7zLegacy: failed to extract file " +
                  std::string(rel_path.begin(), rel_path.end()) +
                  ", res=" + std::to_string(res));
      continue;
    }

    // Build output path, ensure parent directory exists
    std::wstring out_path = output_dir + L"\\" + rel_path;
    auto slash = out_path.rfind(L'\\');
    if (slash != std::wstring::npos) {
      SHCreateDirectoryExW(nullptr, out_path.substr(0, slash).c_str(), nullptr);
    }

    // Write extracted data to disk
    HANDLE hFile = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(hFile, out_buf + offset, (DWORD)out_size_processed, &written, nullptr);
      CloseHandle(hFile);
      extracted_count++;
    } else {
      AddDebugLog("Extract7zLegacy: failed to write file " +
                  std::string(rel_path.begin(), rel_path.end()));
    }
  }

  // --- Cleanup ---
  IAlloc_Free(&g_Alloc, out_buf);
  SzArEx_Free(&db, &g_Alloc);
  CloseHandle(stream.file);

  AddDebugLog("Extraction complete: " + std::to_string(extracted_count) +
              " files written to " + std::string(output_dir.begin(), output_dir.end()));
  return extracted_count > 0;
}
