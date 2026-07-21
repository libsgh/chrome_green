#ifndef CHROME_GREEN_SRC_7Z_EXTRACT_H_
#define CHROME_GREEN_SRC_7Z_EXTRACT_H_

#include <string>

// Extract a .7z archive to output_dir.
// Returns true on success, false on failure.
// Uses the built-in LZMA SDK — no external 7za.exe needed.
bool Extract7zLegacy(const std::wstring& archive, const std::wstring& output_dir);

#endif  // CHROME_GREEN_SRC_7Z_EXTRACT_H_
