//===-- NT string conversion, NLS, and Unicode APIs ---------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_string_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Rtl directory and string conversion functions (ntdll.dll)
//===----------------------------------------------------------------------===//

// Retrieves the process current directory as a wide string.
// Returns the byte count of the directory string (excluding NUL).
// If the buffer is too small, returns the required size in bytes.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR ULONG RtlGetCurrentDirectory_U(ULONG MaximumLength,
                                                            PWCH Buffer);

// Sets the process current directory. Validates the path, opens a directory
// handle for relative path resolution, and updates the PEB atomically under
// the CWD lock.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlSetCurrentDirectory_U(PCUNICODE_STRING PathName);

// Initializes a UNICODE_STRING from a null-terminated wide string.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlInitUnicodeString(PUNICODE_STRING Dest,
                                                       PCWCH Source);

// Initializes a STRING / ANSI_STRING from a null-terminated narrow string.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlInitString(PANSI_STRING Dest, LPCSTR Source);

// Length-checked UNICODE_STRING initializer used by loader paths.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlInitUnicodeStringEx(PUNICODE_STRING Dest, PCWCH Source);

// NTSTATUS <-> Win32 error conversion helpers.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR ULONG RtlNtStatusToDosError(NTSTATUS Status);
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlSetLastWin32Error(ULONG LastError);
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlAppendUnicodeStringToString(PUNICODE_STRING Destination,
                               PCUNICODE_STRING Source);

// Loader entrypoints used by KernelBase loader-facing paths.
// Signatures are mirrored from the local reference tree:
//   F:\Git\llvm\windows-itanium-reference\ntldr.h
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrLoadDll(PCWCH DllPath, PULONG DllCharacteristics, PCUNICODE_STRING DllName,
           PVOID *DllHandle);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrGetDllHandleByName(PCUNICODE_STRING BaseDllName,
                      PCUNICODE_STRING FullDllName, PVOID *DllHandle);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrGetDllHandleByMapping(PVOID BaseAddress, PVOID *DllHandle);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrGetProcedureAddress(PVOID DllHandle, PCANSI_STRING ProcedureName,
                       ULONG ProcedureNumber, PVOID *ProcedureAddress);

inline constexpr ULONG LDR_GET_PROCEDURE_ADDRESS_DONT_RECORD_FORWARDER =
    0x00000001;

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrGetProcedureAddressEx(PVOID DllHandle, PCANSI_STRING ProcedureName,
                         ULONG ProcedureNumber, PVOID *ProcedureAddress,
                         ULONG Flags);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrGetProcedureAddressForCaller(PVOID DllHandle,
                                PCANSI_STRING ProcedureName,
                                ULONG ProcedureNumber,
                                PVOID *ProcedureAddress, ULONG Flags,
                                PVOID CallerAddress);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrGetDllPath(PCWCH DllName, ULONG Flags, PWSTR *DllPath,
              PWSTR *SearchPaths);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlReleasePath(PCWCH Path);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlDosSearchPath_Ustr(ULONG Flags, PCUNICODE_STRING Path,
                      PCUNICODE_STRING FileName,
                      PCUNICODE_STRING DefaultExtension,
                      PUNICODE_STRING StaticString,
                      PUNICODE_STRING DynamicString,
                      PCUNICODE_STRING *FullFileNameOut,
                      PSIZE_T FilePartPrefixCch, PSIZE_T BytesRequired);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlDosApplyFileIsolationRedirection_Ustr(
    ULONG Flags, PCUNICODE_STRING OriginalName, PCUNICODE_STRING Extension,
    PCUNICODE_STRING StaticString, PCUNICODE_STRING DynamicString,
    PCUNICODE_STRING *NewName, PULONG NewFlags, PSIZE_T FileNameSize,
    PSIZE_T RequiredLength);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrAddLoadAsDataTable(PVOID Module, PCWCH FilePath, SIZE_T SizeOfImage,
                      HANDLE FileHandle, PVOID ActivationContext);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LdrRemoveLoadAsDataTable(PVOID DllHandle, PVOID *BaseModule,
                         PSIZE_T FileSize, ULONG Flags);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOLEAN
LdrUnloadAlternateResourceModule(PVOID DllHandle);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS LdrUnloadDll(PVOID DllHandle);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlGetActiveActivationContext(PVOID *ActivationContext);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PVOID RtlImageNtHeader(PVOID BaseOfImage);

// Maps an address to the base of the image (DLL/EXE) containing it.
// Returns the base address, or NULL if the address is not in any loaded image.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PVOID RtlPcToFileHeader(PVOID PcValue,
                                                     PVOID *BaseOfImage);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlImageNtHeaderEx(ULONG Flags, PVOID BaseOfImage, ULONGLONG Size,
                   PVOID *OutHeaders);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
LdrAppxHandleIntegrityFailure(NTSTATUS Status);

// Hash algorithm constants for RtlHashUnicodeString.
inline constexpr ULONG HASH_STRING_ALGORITHM_DEFAULT = 0;
inline constexpr ULONG HASH_STRING_ALGORITHM_X65599 = 1;

// Compute a hash value for a UNICODE_STRING.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlHashUnicodeString(PCUNICODE_STRING String, BOOLEAN CaseInSensitive,
                     ULONG HashAlgorithm, PULONG HashValue);

//===----------------------------------------------------------------------===//
// NLS Locale Data (ntdll.dll)
//===----------------------------------------------------------------------===//

// Maps the system locale blob (l_intl.nls) into the process as a read-only
// section view. The mapping is inherited across fork — no kernel32 state.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtInitializeNlsFiles(PVOID *BaseAddress, PULONG DefaultLocaleId,
                      LARGE_INTEGER *DefaultCasingTableSize,
                      PULONG CurrentNLSVersion);

// Query the system or user default LCID.
// UserProfile=FALSE → system default, TRUE → user default.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryDefaultLocale(BOOLEAN UserProfile, PULONG DefaultLocaleId);

//===----------------------------------------------------------------------===//
// Unicode case mapping and comparison (ntdll.dll)
//===----------------------------------------------------------------------===//

// Single-character uppercase. Uses the 844-format upcase table from the
// NLS blob. Fork-safe — the table is a read-only kernel mapping.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR WCHAR RtlUpcaseUnicodeChar(WCHAR SourceCharacter);

// Single-character lowercase. Same 844-format table.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR WCHAR
RtlDowncaseUnicodeChar(WCHAR SourceCharacter);

// Compare two counted Unicode strings. CaseInSensitive=TRUE folds case.
// Returns negative, zero, or positive (like strcmp).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR LONG
RtlCompareUnicodeStrings(const WCHAR *String1, SIZE_T String1Length,
                          const WCHAR *String2, SIZE_T String2Length,
                          BOOLEAN CaseInSensitive);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_STRING_API_H
