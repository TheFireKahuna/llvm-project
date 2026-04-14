//===-- NT string conversion, NLS, and Unicode APIs ---------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_API_H

#include "src/__support/OSUtil/windows/nt/nt_string_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Rtl directory and string conversion functions (ntdll.dll)
//===----------------------------------------------------------------------===//

// Retrieves the process current directory as a wide string.
// Returns the byte count of the directory string (excluding NUL).
// If the buffer is too small, returns the required size in bytes.
__declspec(dllimport) ULONG NTAPI RtlGetCurrentDirectory_U(ULONG MaximumLength,
                                                            PWCH Buffer);

// Sets the process current directory. Validates the path, opens a directory
// handle for relative path resolution, and updates the PEB atomically under
// the CWD lock.
__declspec(dllimport) NTSTATUS NTAPI
RtlSetCurrentDirectory_U(PCUNICODE_STRING PathName);

// Initializes a UNICODE_STRING from a null-terminated wide string.
__declspec(dllimport) void NTAPI RtlInitUnicodeString(PUNICODE_STRING Dest,
                                                       PCWCH Source);

// Initializes a STRING / ANSI_STRING from a null-terminated narrow string.
__declspec(dllimport) void NTAPI RtlInitString(PANSI_STRING Dest, LPCSTR Source);

// Length-checked UNICODE_STRING initializer used by loader paths.
__declspec(dllimport) NTSTATUS NTAPI
RtlInitUnicodeStringEx(PUNICODE_STRING Dest, PCWCH Source);

// NTSTATUS <-> Win32 error conversion helpers.
__declspec(dllimport) ULONG NTAPI RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) void NTAPI RtlSetLastWin32Error(ULONG LastError);
__declspec(dllimport) NTSTATUS NTAPI
RtlAppendUnicodeStringToString(PUNICODE_STRING Destination,
                               PCUNICODE_STRING Source);

// Loader entrypoints used by KernelBase loader-facing paths.
// Signatures are mirrored from the local reference tree:
//   F:\Git\llvm\windows-itanium-reference\ntldr.h
__declspec(dllimport) NTSTATUS NTAPI
LdrLoadDll(PCWCH DllPath, PULONG DllCharacteristics, PCUNICODE_STRING DllName,
           PVOID *DllHandle);

__declspec(dllimport) NTSTATUS NTAPI
LdrGetDllHandleByName(PCUNICODE_STRING BaseDllName,
                      PCUNICODE_STRING FullDllName, PVOID *DllHandle);

__declspec(dllimport) NTSTATUS NTAPI
LdrGetDllHandleByMapping(PVOID BaseAddress, PVOID *DllHandle);

__declspec(dllimport) NTSTATUS NTAPI
LdrGetProcedureAddress(PVOID DllHandle, PCANSI_STRING ProcedureName,
                       ULONG ProcedureNumber, PVOID *ProcedureAddress);

inline constexpr ULONG LDR_GET_PROCEDURE_ADDRESS_DONT_RECORD_FORWARDER =
    0x00000001;

__declspec(dllimport) NTSTATUS NTAPI
LdrGetProcedureAddressEx(PVOID DllHandle, PCANSI_STRING ProcedureName,
                         ULONG ProcedureNumber, PVOID *ProcedureAddress,
                         ULONG Flags);

__declspec(dllimport) NTSTATUS NTAPI
LdrGetProcedureAddressForCaller(PVOID DllHandle,
                                PCANSI_STRING ProcedureName,
                                ULONG ProcedureNumber,
                                PVOID *ProcedureAddress, ULONG Flags,
                                PVOID CallerAddress);

__declspec(dllimport) NTSTATUS NTAPI
LdrGetDllPath(PCWCH DllName, ULONG Flags, PWSTR *DllPath,
              PWSTR *SearchPaths);

__declspec(dllimport) void NTAPI RtlReleasePath(PCWCH Path);

__declspec(dllimport) NTSTATUS NTAPI
RtlDosSearchPath_Ustr(ULONG Flags, PCUNICODE_STRING Path,
                      PCUNICODE_STRING FileName,
                      PCUNICODE_STRING DefaultExtension,
                      PUNICODE_STRING StaticString,
                      PUNICODE_STRING DynamicString,
                      PCUNICODE_STRING *FullFileNameOut,
                      PSIZE_T FilePartPrefixCch, PSIZE_T BytesRequired);

__declspec(dllimport) NTSTATUS NTAPI
RtlDosApplyFileIsolationRedirection_Ustr(
    ULONG Flags, PCUNICODE_STRING OriginalName, PCUNICODE_STRING Extension,
    PCUNICODE_STRING StaticString, PCUNICODE_STRING DynamicString,
    PCUNICODE_STRING *NewName, PULONG NewFlags, PSIZE_T FileNameSize,
    PSIZE_T RequiredLength);

__declspec(dllimport) NTSTATUS NTAPI
LdrAddLoadAsDataTable(PVOID Module, PCWCH FilePath, SIZE_T SizeOfImage,
                      HANDLE FileHandle, PVOID ActivationContext);

__declspec(dllimport) NTSTATUS NTAPI
LdrRemoveLoadAsDataTable(PVOID DllHandle, PVOID *BaseModule,
                         PSIZE_T FileSize, ULONG Flags);

__declspec(dllimport) BOOLEAN NTAPI
LdrUnloadAlternateResourceModule(PVOID DllHandle);

__declspec(dllimport) NTSTATUS NTAPI LdrUnloadDll(PVOID DllHandle);

__declspec(dllimport) NTSTATUS NTAPI
RtlGetActiveActivationContext(PVOID *ActivationContext);

__declspec(dllimport) PVOID NTAPI RtlImageNtHeader(PVOID BaseOfImage);

// Maps an address to the base of the image (DLL/EXE) containing it.
// Returns the base address, or NULL if the address is not in any loaded image.
__declspec(dllimport) PVOID NTAPI RtlPcToFileHeader(PVOID PcValue,
                                                     PVOID *BaseOfImage);

__declspec(dllimport) NTSTATUS NTAPI
RtlImageNtHeaderEx(ULONG Flags, PVOID BaseOfImage, ULONGLONG Size,
                   PVOID *OutHeaders);

__declspec(dllimport) void NTAPI
LdrAppxHandleIntegrityFailure(NTSTATUS Status);

// Hash algorithm constants for RtlHashUnicodeString.
inline constexpr ULONG HASH_STRING_ALGORITHM_DEFAULT = 0;
inline constexpr ULONG HASH_STRING_ALGORITHM_X65599 = 1;

// Compute a hash value for a UNICODE_STRING.
__declspec(dllimport) NTSTATUS NTAPI
RtlHashUnicodeString(PCUNICODE_STRING String, BOOLEAN CaseInSensitive,
                     ULONG HashAlgorithm, PULONG HashValue);

// Converts a UTF-16 string to UTF-8. Supports a two-pass pattern: call with
// UTF8StringDestination=NULL and UTF8StringMaxByteCount=0 to query the
// required buffer size via *UTF8StringActualByteCount.
__declspec(dllimport) NTSTATUS NTAPI
RtlUnicodeToUTF8N(PCHAR UTF8StringDestination, ULONG UTF8StringMaxByteCount,
                   PULONG UTF8StringActualByteCount,
                   PCWCH UnicodeStringSource, ULONG UnicodeStringByteCount);

// Converts a UTF-8 string to UTF-16. Same two-pass pattern as above.
__declspec(dllimport) NTSTATUS NTAPI
RtlUTF8ToUnicodeN(PWCH UnicodeStringDestination,
                   ULONG UnicodeStringMaxByteCount,
                   PULONG UnicodeStringActualByteCount,
                   const CHAR *UTF8StringSource,
                   ULONG UTF8StringByteCount);

//===----------------------------------------------------------------------===//
// NLS Locale Data (ntdll.dll)
//===----------------------------------------------------------------------===//

// Maps the system locale blob (l_intl.nls) into the process as a read-only
// section view. The mapping is inherited across fork — no kernel32 state.
__declspec(dllimport) NTSTATUS NTAPI
NtInitializeNlsFiles(PVOID *BaseAddress, PULONG DefaultLocaleId,
                      LARGE_INTEGER *DefaultCasingTableSize,
                      PULONG CurrentNLSVersion);

// Query the system or user default LCID.
// UserProfile=FALSE → system default, TRUE → user default.
__declspec(dllimport) NTSTATUS NTAPI
NtQueryDefaultLocale(BOOLEAN UserProfile, PULONG DefaultLocaleId);

//===----------------------------------------------------------------------===//
// Unicode case mapping and comparison (ntdll.dll)
//===----------------------------------------------------------------------===//

// Single-character uppercase. Uses the 844-format upcase table from the
// NLS blob. Fork-safe — the table is a read-only kernel mapping.
__declspec(dllimport) WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR SourceCharacter);

// Single-character lowercase. Same 844-format table.
__declspec(dllimport) WCHAR NTAPI
RtlDowncaseUnicodeChar(WCHAR SourceCharacter);

// Compare two counted Unicode strings. CaseInSensitive=TRUE folds case.
// Returns negative, zero, or positive (like strcmp).
__declspec(dllimport) LONG NTAPI
RtlCompareUnicodeStrings(const WCHAR *String1, SIZE_T String1Length,
                          const WCHAR *String2, SIZE_T String2Length,
                          BOOLEAN CaseInSensitive);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_API_H
