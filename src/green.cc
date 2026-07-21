#include "green.h"

#include <windows.h>

#include <lmaccess.h>
#include <processthreadsapi.h>
#include <shlwapi.h>

#include "detours.h"

#include "config.h"
#include "utils.h"

namespace {

// Static function pointers for original functions
static auto RawUpdateProcThreadAttribute = UpdateProcThreadAttribute;
static auto RawCryptProtectData = CryptProtectData;
static auto RawCryptUnprotectData = CryptUnprotectData;
static auto RawGetVolumeInformationW = GetVolumeInformationW;
static auto RawLogonUserW = LogonUserW;
static auto RawIsOS = IsOS;
static auto RawNetUserGetInfo = NetUserGetInfo;

// Enumeration for process creation mitigation policy
enum ProcessCreationMitigationPolicy : DWORD64 {
  BlockNonMicrosoftBinariesAlwaysOn = 0x00000001ui64 << 44,
  Win32kSystemCallDisableAlwaysOn = 0x00000001ui64 << 28
};

BOOL WINAPI FakeGetComputerName(_Out_ LPTSTR lpBuffer,
                                _Inout_ LPDWORD lpnSize) {
  return false;
}

// Mirrors Chromium's portability hack: when lpVolumeSerialNumber is requested
// we return FALSE so the volume serial is never exposed. Reference:
// https://source.chromium.org/chromium/chromium/src/+/main:rlz/win/lib/machine_id_win.cc;l=41;drc=3dd5eb19b88fb0246061e21fc6098830bead0edb
//
// When lpVolumeSerialNumber is null we forward to RawGetVolumeInformationW,
// because other code paths still need real volume info (e.g.
// lpMaximumComponentLength). Reference:
// https://source.chromium.org/chromium/chromium/src/+/main:base/files/file_util_win.cc;drc=5b01e9f5bff328ba66e415103ca50ae940328fde;l=1071
//
// Returns FALSE if lpVolumeSerialNumber is non-null; otherwise the
// RawGetVolumeInformationW result.
BOOL WINAPI FakeGetVolumeInformation(_In_opt_ LPCTSTR lpRootPathName,
                                     _Out_opt_ LPTSTR lpVolumeNameBuffer,
                                     _In_ DWORD nVolumeNameSize,
                                     _Out_opt_ LPDWORD lpVolumeSerialNumber,
                                     _Out_opt_ LPDWORD lpMaximumComponentLength,
                                     _Out_opt_ LPDWORD lpFileSystemFlags,
                                     _Out_opt_ LPTSTR lpFileSystemNameBuffer,
                                     _In_ DWORD nFileSystemNameSize) {
  if (lpVolumeSerialNumber != nullptr) {
    return false;
  } else {
    return RawGetVolumeInformationW(
        lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize,
        lpVolumeSerialNumber, lpMaximumComponentLength, lpFileSystemFlags,
        lpFileSystemNameBuffer, nFileSystemNameSize);
  }
}

BOOL WINAPI MyUpdateProcThreadAttribute(
    __inout LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    __in DWORD dwFlags,
    __in DWORD_PTR Attribute,
    __in_bcount_opt(cbSize) PVOID lpValue,
    __in SIZE_T cbSize,
    __out_bcount_opt(cbSize) PVOID lpPreviousValue,
    __in_opt PSIZE_T lpReturnSize) {
  if (Attribute == PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY &&
      cbSize >= sizeof(DWORD64)) {
    // https://source.chromium.org/chromium/chromium/src/+/main:sandbox/win/src/process_mitigations.cc;l=362;drc=4c2fec5f6699ffeefd93137d2bf8c03504c6664c
    PDWORD64 policy_value_1 = &(static_cast<PDWORD64>(lpValue))[0];
    *policy_value_1 &= ~static_cast<DWORD64>(
        ProcessCreationMitigationPolicy::BlockNonMicrosoftBinariesAlwaysOn);
    if (config.IsWin32K()) {
      *policy_value_1 &= ~static_cast<DWORD64>(
          ProcessCreationMitigationPolicy::Win32kSystemCallDisableAlwaysOn);
    }
  }
  return RawUpdateProcThreadAttribute(lpAttributeList, dwFlags, Attribute,
                                      lpValue, cbSize, lpPreviousValue,
                                      lpReturnSize);
}

BOOL WINAPI
MyCryptProtectData(_In_ DATA_BLOB* pDataIn,
                   _In_opt_ LPCWSTR szDataDescr,
                   _In_opt_ DATA_BLOB* pOptionalEntropy,
                   _Reserved_ PVOID pvReserved,
                   _In_opt_ CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct,
                   _In_ DWORD dwFlags,
                   _Out_ DATA_BLOB* pDataOut) {
  pDataOut->cbData = pDataIn->cbData;
  pDataOut->pbData =
      static_cast<BYTE*>(LocalAlloc(LMEM_FIXED, pDataOut->cbData));
  memcpy(pDataOut->pbData, pDataIn->pbData, pDataOut->cbData);
  return true;
}

BOOL WINAPI
MyCryptUnprotectData(_In_ DATA_BLOB* pDataIn,
                     _Out_opt_ LPWSTR* ppszDataDescr,
                     _In_opt_ DATA_BLOB* pOptionalEntropy,
                     _Reserved_ PVOID pvReserved,
                     _In_opt_ CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct,
                     _In_ DWORD dwFlags,
                     _Out_ DATA_BLOB* pDataOut) {
  if (RawCryptUnprotectData(pDataIn, ppszDataDescr, pOptionalEntropy,
                            pvReserved, pPromptStruct, dwFlags, pDataOut)) {
    return true;
  }

  pDataOut->cbData = pDataIn->cbData;
  pDataOut->pbData =
      static_cast<BYTE*>(LocalAlloc(LMEM_FIXED, pDataOut->cbData));
  memcpy(pDataOut->pbData, pDataIn->pbData, pDataOut->cbData);
  return true;
}

}  // namespace

BOOL WINAPI MyLogonUserW(LPCWSTR lpszUsername,
                         LPCWSTR lpszDomain,
                         LPCWSTR lpszPassword,
                         DWORD dwLogonType,
                         DWORD dwLogonProvider,
                         PHANDLE phToken) {
  BOOL ret = RawLogonUserW(lpszUsername, lpszDomain, lpszPassword, dwLogonType,
                           dwLogonProvider, phToken);
  // Chrome asks the user to re-enter their Windows password before revealing
  // saved passwords. Pretending the supplied credentials always "worked"
  // (while keeping the real result) lets the password manager skip that OS
  // reauth gate. This is the show_password feature.
  SetLastError(ERROR_ACCOUNT_RESTRICTION);
  return ret;
}

BOOL WINAPI MyIsOS(DWORD dwOS) {
  DWORD ret = RawIsOS(dwOS);
  // Chrome's password reauth checks whether the machine is domain-joined;
  // report "not a domain member" so it takes the local-account path.
  if (dwOS == OS_DOMAINMEMBER) {
    return false;
  }
  return ret;
}

NET_API_STATUS WINAPI MyNetUserGetInfo(LPCWSTR servername,
                                       LPCWSTR username,
                                       DWORD level,
                                       LPBYTE* bufptr) {
  NET_API_STATUS ret = RawNetUserGetInfo(servername, username, level, bufptr);
  // Force the account's password age to 0 so Chrome believes the password was
  // just changed and waives the "password is old" reauth requirement.
  if (level == 1 && ret == 0) {
    LPUSER_INFO_1 user_info = reinterpret_cast<LPUSER_INFO_1>(*bufptr);
    user_info->usri1_password_age = 0;
  }
  return ret;
}

void MakeGreen() {
  auto RawGetComputerNameW = GetComputerNameW;

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());

  // kernel32.dll
  DetourAttach(reinterpret_cast<LPVOID*>(&RawGetComputerNameW),
               reinterpret_cast<void*>(FakeGetComputerName));
  DetourAttach(reinterpret_cast<LPVOID*>(&RawGetVolumeInformationW),
               reinterpret_cast<void*>(FakeGetVolumeInformation));
  DetourAttach(reinterpret_cast<LPVOID*>(&RawUpdateProcThreadAttribute),
               reinterpret_cast<void*>(MyUpdateProcThreadAttribute));

  // components/os_crypt/os_crypt_win.cc
  // crypt32.dll
  DetourAttach(reinterpret_cast<LPVOID*>(&RawCryptProtectData),
               reinterpret_cast<void*>(MyCryptProtectData));
  DetourAttach(reinterpret_cast<LPVOID*>(&RawCryptUnprotectData),
               reinterpret_cast<void*>(MyCryptUnprotectData));

  if (config.IsShowPassword()) {
    // advapi32.dll
    DetourAttach(reinterpret_cast<LPVOID*>(&RawLogonUserW),
                 reinterpret_cast<void*>(MyLogonUserW));
    // shlwapi.dll
    DetourAttach(reinterpret_cast<LPVOID*>(&RawIsOS),
                 reinterpret_cast<void*>(MyIsOS));
    // netapi32.dll
    DetourAttach(reinterpret_cast<LPVOID*>(&RawNetUserGetInfo),
                 reinterpret_cast<void*>(MyNetUserGetInfo));
  }

  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"MakeGreen failed: {}", status);
  }
}
