//===-- Libc-internal Windows types for NT API declarations ---- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Libc-internal NT types and constants. Base scalar types, calling conventions,
// and SEH structures come from <sys/ntabi.h> (the public platform ABI header).
// This file adds libc-specific types: NTSTATUS codes, UNICODE_STRING,
// OBJECT_ATTRIBUTES, IO_STATUS_BLOCK, string/handle types, etc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_TYPES_H

#include "include/sys/ntabi.h"

// Macro for __builtin_offsetof — matches the SDK's FIELD_OFFSET.
#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) __builtin_offsetof(type, field)
#endif

//===----------------------------------------------------------------------===//
// Additional Pointer / String Types (libc-internal)
//===----------------------------------------------------------------------===//

// These are not needed by the public ABI structures but are used extensively
// by libc's NT API wrappers.
using PVOID64 = void *; // Always 8 bytes on x64.
using PUCHAR = unsigned char *;
using PCHAR = char *;
using PCSTR = const char *;
using LPCSTR = const char *;
using PUSHORT = unsigned short *;
// Win32 WCHAR is always 16-bit UTF-16 regardless of the compiler's wchar_t
// WCHAR, LPCWSTR, LPWSTR are now in <sys/ntabi.h>.
using PWSTR = char16_t *;
using PULONG = unsigned long *;
using PLONG = long *;
using PULONG_PTR = ULONG_PTR *;
using PSIZE_T = SIZE_T *;
using UINT = unsigned int;
using LANGID = USHORT;

// Win32 handle types — opaque pointers.
using HWND = void *;
using HHOOK = void *;

// Win32 compatibility constants that are part of the basic type layer rather
// than a higher-level DLL surface.
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR) - 1)
inline constexpr BOOL FALSE = 0;
inline constexpr BOOL TRUE = 1;

// Win32 message parameter types.
using WPARAM = ULONG_PTR;
using LPARAM = LONG_PTR;

// Common Win32 path limit used by APIs that still operate on DOS paths.
// TODO: Remove remaining libc call sites that treat this as a general Windows
// path limit. Windows 11 long-path-aware code should not normalize MAX_PATH as
// the steady-state bound for path handling.
inline constexpr ULONG MAX_PATH = 260;

// Common Win32 error codes used by compatibility shims that translate TEB
// LastError values into POSIX errno.
inline constexpr DWORD ERROR_SUCCESS = 0;
inline constexpr DWORD ERROR_INVALID_HANDLE = 6;
inline constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8;
inline constexpr DWORD ERROR_OUTOFMEMORY = 14;
inline constexpr DWORD ERROR_INVALID_PARAMETER = 87;
inline constexpr DWORD ERROR_INVALID_NAME = 123;
inline constexpr DWORD ERROR_ACCESS_DENIED = 5;
inline constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
inline constexpr DWORD ERROR_PATH_NOT_FOUND = 3;
inline constexpr DWORD ERROR_FILE_EXISTS = 80;
inline constexpr DWORD ERROR_ALREADY_EXISTS = 183;
inline constexpr DWORD ERROR_BROKEN_PIPE = 109;
inline constexpr DWORD ERROR_NOT_SUPPORTED = 50;
inline constexpr DWORD ERROR_TIMEOUT = 1460;
inline constexpr DWORD ERROR_BUSY = 170;
inline constexpr DWORD ERROR_NOT_LOCKED = 158;
inline constexpr DWORD ERROR_WORKING_SET_QUOTA = 1453;
inline constexpr DWORD ERROR_INVALID_ADDRESS = 487;
inline constexpr DWORD ERROR_SHARING_VIOLATION = 32;
inline constexpr DWORD ERROR_BAD_EXE_FORMAT = 193;
inline constexpr DWORD ERROR_FILENAME_EXCED_RANGE = 206;
inline constexpr DWORD ERROR_DIRECTORY = 267;
inline constexpr DWORD ERROR_CANT_RESOLVE_FILENAME = 1921;

// Coordinate pair — used by console APIs and ConDrv protocol messages.
struct COORD {
  SHORT X;
  SHORT Y;
};

// Handle types
using HMODULE = void *;
// Slim reader/writer lock — a single pointer-sized atomic.
// Value 0 = unlocked; low bits encode exclusive/shared/waiter state.
struct SRWLOCK {
  PVOID Ptr;
};
using RTL_SRWLOCK = SRWLOCK;
using PRTL_SRWLOCK = SRWLOCK *;

// Function pointer — return type of GetProcAddress / LdrGetProcedureAddress.
// The loader returns pointers to functions that follow MS x64 ABI, so the
// target function type must be pinned via LIBC_MSABI regardless of TU
// default. LIBC_MSABI can legally appertain to a function *declaration*
// but not a function *type* (the trailing-alias form is rejected by GCC
// and warned by Clang). We therefore synthesize the pointer type via
// `decltype` on an unreferenced external prototype — never defined,
// never odr-used (decltype is unevaluated), so the linker discards it.
LIBC_MSABI long long __farproc_type_source();
using FARPROC = decltype(&__farproc_type_source);

//===----------------------------------------------------------------------===//
// NTSTATUS Codes
//===----------------------------------------------------------------------===//

inline constexpr NTSTATUS STATUS_SUCCESS = 0x00000000;
inline constexpr NTSTATUS STATUS_ALERTED = 0x00000101;
inline constexpr NTSTATUS STATUS_TIMEOUT = 0x00000102;
inline constexpr NTSTATUS STATUS_CANCELLED = static_cast<NTSTATUS>(0xC0000120);
inline constexpr NTSTATUS STATUS_BUFFER_OVERFLOW =
    static_cast<NTSTATUS>(0x80000005);
inline constexpr NTSTATUS STATUS_OBJECT_NAME_EXISTS = 0x40000000;
// Note: NtLockVirtualMemory returns STATUS_SUCCESS even if pages are already
// locked. There is no separate "already locked" informational status.
inline constexpr NTSTATUS STATUS_INVALID_HANDLE =
    static_cast<NTSTATUS>(0xC0000008);
inline constexpr NTSTATUS STATUS_INVALID_PARAMETER =
    static_cast<NTSTATUS>(0xC000000D);
inline constexpr NTSTATUS STATUS_INVALID_DEVICE_REQUEST =
    static_cast<NTSTATUS>(0xC0000010);
inline constexpr NTSTATUS STATUS_NO_SUCH_FILE =
    static_cast<NTSTATUS>(0xC000000F);
inline constexpr NTSTATUS STATUS_NO_MEMORY = static_cast<NTSTATUS>(0xC0000017);
inline constexpr NTSTATUS STATUS_ACCESS_DENIED =
    static_cast<NTSTATUS>(0xC0000022);
inline constexpr NTSTATUS STATUS_INVALID_CID =
    static_cast<NTSTATUS>(0xC000000B);
inline constexpr NTSTATUS STATUS_NOT_LOCKED = static_cast<NTSTATUS>(0xC000002A);
inline constexpr NTSTATUS STATUS_OBJECT_NAME_NOT_FOUND =
    static_cast<NTSTATUS>(0xC0000034);
inline constexpr NTSTATUS STATUS_OBJECT_NAME_COLLISION =
    static_cast<NTSTATUS>(0xC0000035);
inline constexpr NTSTATUS STATUS_CONFLICTING_ADDRESSES =
    static_cast<NTSTATUS>(0xC0000018);
inline constexpr NTSTATUS STATUS_NOT_MAPPED_VIEW =
    static_cast<NTSTATUS>(0xC0000019);
// Returned by NtFreeVirtualMemory / placeholder conversion when the region
// is already free or already a placeholder. Used in mmap MAP_FIXED retry logic.
inline constexpr NTSTATUS STATUS_UNABLE_TO_FREE_VM =
    static_cast<NTSTATUS>(0xC000001A);
inline constexpr NTSTATUS STATUS_INVALID_VIEW_SIZE =
    static_cast<NTSTATUS>(0xC000001F);
inline constexpr NTSTATUS STATUS_INVALID_PAGE_PROTECTION =
    static_cast<NTSTATUS>(0xC0000045);
// Returned by NtProtectVirtualMemory when the requested protection exceeds
// the access mask the file section was created with (e.g., READWRITE on a
// read-only file mapping). Distinct from STATUS_INVALID_PAGE_PROTECTION which
// covers invalid flag combinations and SEC_NO_CHANGE sections.
inline constexpr NTSTATUS STATUS_SECTION_PROTECTION =
    static_cast<NTSTATUS>(0xC000004E);
// Returned by NtProtectVirtualMemory when ACG (Arbitrary Code Guard) is active
// and the requested protection change would violate W^X policy: making an
// executable page writable, or making a writable page executable, is blocked.
// Maps to EACCES to distinguish from invalid-argument errors.
inline constexpr NTSTATUS STATUS_DYNAMIC_CODE_BLOCKED =
    static_cast<NTSTATUS>(0xC0000604);
inline constexpr NTSTATUS STATUS_WORKING_SET_QUOTA =
    static_cast<NTSTATUS>(0xC00000A1);
inline constexpr NTSTATUS STATUS_ALREADY_COMMITTED =
    static_cast<NTSTATUS>(0xC0000021);
inline constexpr NTSTATUS STATUS_UNSUCCESSFUL =
    static_cast<NTSTATUS>(0xC0000001);
inline constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH =
    static_cast<NTSTATUS>(0xC0000004);
inline constexpr NTSTATUS STATUS_INSUFFICIENT_RESOURCES =
    static_cast<NTSTATUS>(0xC000009A);
inline constexpr NTSTATUS STATUS_COMMITMENT_LIMIT =
    static_cast<NTSTATUS>(0xC000012D);
inline constexpr NTSTATUS STATUS_GUARD_PAGE_VIOLATION =
    static_cast<NTSTATUS>(0x80000001);

// Process cloning — returned by NtCreateUserProcess in the child (cloned)
// thread when invoked without a filename/section (fork mode).
inline constexpr NTSTATUS STATUS_PROCESS_CLONED = 0x00000129;

// File I/O status codes
inline constexpr NTSTATUS STATUS_USER_APC = 0x000000C0;
inline constexpr NTSTATUS STATUS_PENDING = 0x00000103;
inline constexpr NTSTATUS STATUS_NO_MORE_FILES =
    static_cast<NTSTATUS>(0x80000006);
inline constexpr NTSTATUS STATUS_END_OF_FILE =
    static_cast<NTSTATUS>(0xC0000011);
inline constexpr NTSTATUS STATUS_SHARING_VIOLATION =
    static_cast<NTSTATUS>(0xC0000043);
inline constexpr NTSTATUS STATUS_OBJECT_PATH_NOT_FOUND =
    static_cast<NTSTATUS>(0xC000003A);
inline constexpr NTSTATUS STATUS_FILE_IS_A_DIRECTORY =
    static_cast<NTSTATUS>(0xC00000Ba);
inline constexpr NTSTATUS STATUS_NOT_A_DIRECTORY =
    static_cast<NTSTATUS>(0xC0000103);
inline constexpr NTSTATUS STATUS_NAME_TOO_LONG =
    static_cast<NTSTATUS>(0xC0000106);
inline constexpr NTSTATUS STATUS_DIRECTORY_NOT_EMPTY =
    static_cast<NTSTATUS>(0xC0000101);
inline constexpr NTSTATUS STATUS_NOT_SAME_DEVICE =
    static_cast<NTSTATUS>(0xC00000D4);
inline constexpr NTSTATUS STATUS_CANNOT_DELETE =
    static_cast<NTSTATUS>(0xC0000121);
inline constexpr NTSTATUS STATUS_NOT_A_REPARSE_POINT =
    static_cast<NTSTATUS>(0xC0000275);
inline constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL =
    static_cast<NTSTATUS>(0xC0000023);
inline constexpr NTSTATUS STATUS_PRIVILEGE_NOT_HELD =
    static_cast<NTSTATUS>(0xC0000061);
inline constexpr NTSTATUS STATUS_NO_SUCH_USER =
    static_cast<NTSTATUS>(0xC0000064);
inline constexpr NTSTATUS STATUS_STOPPED_ON_SYMLINK =
    static_cast<NTSTATUS>(0x8000002D);
inline constexpr NTSTATUS STATUS_PIPE_DISCONNECTED =
    static_cast<NTSTATUS>(0xC00000B0);
inline constexpr NTSTATUS STATUS_PIPE_BROKEN =
    static_cast<NTSTATUS>(0xC000014B);
inline constexpr NTSTATUS STATUS_PIPE_EMPTY = static_cast<NTSTATUS>(0xC00000D9);
inline constexpr NTSTATUS STATUS_PIPE_CONNECTED =
    static_cast<NTSTATUS>(0x00000003);
inline constexpr NTSTATUS STATUS_PIPE_CLOSING =
    static_cast<NTSTATUS>(0xC00000B1);
inline constexpr NTSTATUS STATUS_PIPE_NOT_AVAILABLE =
    static_cast<NTSTATUS>(0xC00000AC);
inline constexpr NTSTATUS STATUS_PIPE_LISTENING =
    static_cast<NTSTATUS>(0xC00000B3);
inline constexpr NTSTATUS STATUS_INSTANCE_NOT_AVAILABLE =
    static_cast<NTSTATUS>(0xC00000AB);
inline constexpr NTSTATUS STATUS_TOO_MANY_LINKS =
    static_cast<NTSTATUS>(0xC0000265);
inline constexpr NTSTATUS STATUS_FILE_TOO_LARGE =
    static_cast<NTSTATUS>(0xC0000904);
inline constexpr NTSTATUS STATUS_DISK_FULL = static_cast<NTSTATUS>(0xC000007F);
inline constexpr NTSTATUS STATUS_MEDIA_WRITE_PROTECTED =
    static_cast<NTSTATUS>(0xC00000A2);
inline constexpr NTSTATUS STATUS_DELETE_PENDING =
    static_cast<NTSTATUS>(0xC0000056);
inline constexpr NTSTATUS STATUS_NOTIFY_CLEANUP =
    static_cast<NTSTATUS>(0x0000010B);
inline constexpr NTSTATUS STATUS_NOTIFY_ENUM_DIR =
    static_cast<NTSTATUS>(0x0000010C);
inline constexpr NTSTATUS STATUS_FILE_CLOSED =
    static_cast<NTSTATUS>(0xC0000128);
inline constexpr NTSTATUS STATUS_OBJECT_NAME_INVALID =
    static_cast<NTSTATUS>(0xC0000033);
inline constexpr NTSTATUS STATUS_OBJECT_PATH_SYNTAX_BAD =
    static_cast<NTSTATUS>(0xC000003B);
inline constexpr NTSTATUS STATUS_NOT_SUPPORTED =
    static_cast<NTSTATUS>(0xC00000BB);
inline constexpr NTSTATUS STATUS_DLL_NOT_FOUND =
    static_cast<NTSTATUS>(0xC0000135);
inline constexpr NTSTATUS STATUS_ORDINAL_NOT_FOUND =
    static_cast<NTSTATUS>(0xC0000138);
inline constexpr NTSTATUS STATUS_ENTRYPOINT_NOT_FOUND =
    static_cast<NTSTATUS>(0xC0000139);
inline constexpr NTSTATUS STATUS_INVALID_IMAGE_FORMAT =
    static_cast<NTSTATUS>(0xC000007B);
inline constexpr NTSTATUS STATUS_INVALID_IMAGE_NOT_MZ =
    static_cast<NTSTATUS>(0xC000012F);
inline constexpr NTSTATUS STATUS_INVALID_IMAGE_NE_FORMAT =
    static_cast<NTSTATUS>(0xC000011B);
inline constexpr NTSTATUS STATUS_INVALID_IMAGE_PROTECT =
    static_cast<NTSTATUS>(0xC0000130);
inline constexpr NTSTATUS STATUS_NEEDS_REMEDIATION =
    static_cast<NTSTATUS>(0xC0000462);
inline constexpr NTSTATUS STATUS_SYSTEM_NEEDS_REMEDIATION =
    static_cast<NTSTATUS>(0xC000047E);
inline constexpr NTSTATUS STATUS_APPX_INTEGRITY_FAILURE_CLR_NGEN =
    static_cast<NTSTATUS>(0xC000047F);
// Observed in KernelBase!LoadLibraryExW's side-by-side fallback path after
// RtlDosApplyFileIsolationRedirection_Ustr fails.
inline constexpr NTSTATUS STATUS_SXS_KEY_NOT_FOUND =
    static_cast<NTSTATUS>(0xC0150008);

// Socket / connection status codes (AFD, tcpip).
inline constexpr NTSTATUS STATUS_CONNECTION_REFUSED =
    static_cast<NTSTATUS>(0xC0000236);
inline constexpr NTSTATUS STATUS_CONNECTION_RESET =
    static_cast<NTSTATUS>(0xC000020D);
inline constexpr NTSTATUS STATUS_CONNECTION_ABORTED =
    static_cast<NTSTATUS>(0xC0000241);
inline constexpr NTSTATUS STATUS_CONNECTION_DISCONNECTED =
    static_cast<NTSTATUS>(0xC000020C);
inline constexpr NTSTATUS STATUS_GRACEFUL_DISCONNECT =
    static_cast<NTSTATUS>(0xC0000237);
inline constexpr NTSTATUS STATUS_ADDRESS_ALREADY_EXISTS =
    static_cast<NTSTATUS>(0xC000020A);
inline constexpr NTSTATUS STATUS_ADDRESS_CLOSED =
    static_cast<NTSTATUS>(0xC0000238);
inline constexpr NTSTATUS STATUS_CONNECTION_INVALID =
    static_cast<NTSTATUS>(0xC000023A);
inline constexpr NTSTATUS STATUS_NETWORK_UNREACHABLE =
    static_cast<NTSTATUS>(0xC000023C);
inline constexpr NTSTATUS STATUS_HOST_UNREACHABLE =
    static_cast<NTSTATUS>(0xC000023D);
inline constexpr NTSTATUS STATUS_PORT_UNREACHABLE =
    static_cast<NTSTATUS>(0xC000023F);
inline constexpr NTSTATUS STATUS_IO_TIMEOUT = static_cast<NTSTATUS>(0xC00000B5);
inline constexpr NTSTATUS STATUS_DEVICE_NOT_READY =
    static_cast<NTSTATUS>(0xC00000A3);
inline constexpr NTSTATUS STATUS_LOCAL_DISCONNECT =
    static_cast<NTSTATUS>(0xC000013B);
inline constexpr NTSTATUS STATUS_REMOTE_DISCONNECT =
    static_cast<NTSTATUS>(0xC000013C);
inline constexpr NTSTATUS STATUS_LOCK_NOT_GRANTED =
    static_cast<NTSTATUS>(0xC0000055);
inline constexpr NTSTATUS STATUS_FILE_LOCK_CONFLICT =
    static_cast<NTSTATUS>(0xC0000054);
inline constexpr NTSTATUS STATUS_INVALID_USER_BUFFER =
    static_cast<NTSTATUS>(0xC00000E8);
inline constexpr NTSTATUS STATUS_RANGE_NOT_LOCKED =
    static_cast<NTSTATUS>(0xC000007E);

// Additional NT-specific codes for comprehensive errno mapping.
inline constexpr NTSTATUS STATUS_ACCESS_VIOLATION =
    static_cast<NTSTATUS>(0xC0000005);
inline constexpr NTSTATUS STATUS_NOT_IMPLEMENTED =
    static_cast<NTSTATUS>(0xC0000002);
inline constexpr NTSTATUS STATUS_INVALID_SYSTEM_SERVICE =
    static_cast<NTSTATUS>(0xC000001C);
inline constexpr NTSTATUS STATUS_ILLEGAL_INSTRUCTION =
    static_cast<NTSTATUS>(0xC000001D);
inline constexpr NTSTATUS STATUS_THREAD_IS_TERMINATING =
    static_cast<NTSTATUS>(0xC000004B);
inline constexpr NTSTATUS STATUS_PROCESS_IS_TERMINATING =
    static_cast<NTSTATUS>(0xC000000A);
inline constexpr NTSTATUS STATUS_INTEGER_OVERFLOW =
    static_cast<NTSTATUS>(0xC0000095);
inline constexpr NTSTATUS STATUS_OBJECT_TYPE_MISMATCH =
    static_cast<NTSTATUS>(0xC0000024);
inline constexpr NTSTATUS STATUS_ILLEGAL_FUNCTION =
    static_cast<NTSTATUS>(0xC00000AF);
inline constexpr NTSTATUS STATUS_NOT_FOUND =
    static_cast<NTSTATUS>(0xC0000225);
inline constexpr NTSTATUS STATUS_TOO_MANY_OPENED_FILES =
    static_cast<NTSTATUS>(0xC000011F);
inline constexpr NTSTATUS STATUS_FILE_CORRUPT_ERROR =
    static_cast<NTSTATUS>(0xC0000102);
inline constexpr NTSTATUS STATUS_DISK_CORRUPT_ERROR =
    static_cast<NTSTATUS>(0xC0000032);
inline constexpr NTSTATUS STATUS_QUOTA_EXCEEDED =
    static_cast<NTSTATUS>(0xC0000044);
inline constexpr NTSTATUS STATUS_DEVICE_BUSY =
    static_cast<NTSTATUS>(0x80000011);
inline constexpr NTSTATUS STATUS_TOO_MANY_PAGING_FILES =
    static_cast<NTSTATUS>(0xC0000097);
// STATUS_MUTANT_NOT_OWNED defined in nt_process_types.h.
// STATUS_SEMAPHORE_LIMIT_EXCEEDED defined in nt_process_types.h.
// STATUS_PORT_CONNECTION_REFUSED defined in nt_ipc_types.h.
inline constexpr NTSTATUS STATUS_OBJECT_PATH_INVALID =
    static_cast<NTSTATUS>(0xC0000039);
inline constexpr NTSTATUS STATUS_TOO_MANY_THREADS =
    static_cast<NTSTATUS>(0xC0000129);
inline constexpr NTSTATUS STATUS_SECTION_NOT_EXTENDED =
    static_cast<NTSTATUS>(0xC0000087);
inline constexpr NTSTATUS STATUS_FILE_RENAMED =
    static_cast<NTSTATUS>(0xC00000D5);
inline constexpr NTSTATUS STATUS_NETWORK_NAME_DELETED =
    static_cast<NTSTATUS>(0xC00000C9);
inline constexpr NTSTATUS STATUS_LOGON_FAILURE =
    static_cast<NTSTATUS>(0xC000006D);
inline constexpr NTSTATUS STATUS_ACCOUNT_RESTRICTION =
    static_cast<NTSTATUS>(0xC000006E);
inline constexpr NTSTATUS STATUS_WRONG_PASSWORD =
    static_cast<NTSTATUS>(0xC000006A);
inline constexpr NTSTATUS STATUS_ACCOUNT_DISABLED =
    static_cast<NTSTATUS>(0xC0000072);
inline constexpr NTSTATUS STATUS_ACCOUNT_EXPIRED =
    static_cast<NTSTATUS>(0xC0000193);
inline constexpr NTSTATUS STATUS_PASSWORD_EXPIRED =
    static_cast<NTSTATUS>(0xC0000071);

// NTSTATUS severity bit — bit 31 set indicates error severity. Used to
// fast-path VEH exception filtering: all hardware exceptions have this bit
// set, while most language runtime exceptions (Itanium C++, Delphi) do not.
inline constexpr ULONG NTSTATUS_SEVERITY_ERROR_BIT = 0x80000000;

// Windows exception codes — software exceptions raised by language runtimes.
// Used to filter VEH before POSIX signal dispatch.
//   0xE06D7363: MSVC C++ (_CxxThrowException — "msc" in ASCII)
//   0xE0434352: .NET CLR managed exception ("CCR" in ASCII)
inline constexpr DWORD MSVC_CPP_EXCEPTION_CODE = 0xE06D7363;
inline constexpr DWORD CLR_EXCEPTION_CODE = 0xE0434352;

// Helper macros
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#define NT_ERROR(Status) ((NTSTATUS)(Status) < 0)

// UNW_FLAG_* constants provided by <sys/ntabi.h>.

//===----------------------------------------------------------------------===//
// Pointer / Derived Types
//===----------------------------------------------------------------------===//

using PHANDLE = HANDLE *;
using ACCESS_MASK = ULONG;

//===----------------------------------------------------------------------===//
// Standard Access Rights
//===----------------------------------------------------------------------===//

inline constexpr ACCESS_MASK DELETE_ACCESS = 0x00010000;
inline constexpr ACCESS_MASK READ_CONTROL = 0x00020000;
inline constexpr ACCESS_MASK WRITE_DAC = 0x00040000;
inline constexpr ACCESS_MASK WRITE_OWNER = 0x00080000;
inline constexpr ACCESS_MASK SYNCHRONIZE = 0x00100000;

inline constexpr ACCESS_MASK STANDARD_RIGHTS_REQUIRED = 0x000F0000;
inline constexpr ACCESS_MASK STANDARD_RIGHTS_READ = READ_CONTROL;
inline constexpr ACCESS_MASK STANDARD_RIGHTS_WRITE = READ_CONTROL;
inline constexpr ACCESS_MASK STANDARD_RIGHTS_EXECUTE = READ_CONTROL;
inline constexpr ACCESS_MASK STANDARD_RIGHTS_ALL = 0x001F0000;
inline constexpr ACCESS_MASK SPECIFIC_RIGHTS_ALL = 0x0000FFFF;

inline constexpr ACCESS_MASK ACCESS_SYSTEM_SECURITY = 0x01000000;
inline constexpr ACCESS_MASK MAXIMUM_ALLOWED = 0x02000000;

inline constexpr ACCESS_MASK GENERIC_READ = 0x80000000;
inline constexpr ACCESS_MASK GENERIC_WRITE = 0x40000000;
inline constexpr ACCESS_MASK GENERIC_EXECUTE = 0x20000000;
inline constexpr ACCESS_MASK GENERIC_ALL = 0x10000000;
using PWCH = WCHAR *;
using PCWCH = const WCHAR *;
using CCHAR = char;
using CSHORT = short;

// Native APC callback used by NtQueueApcThread* entrypoints. Same
// `decltype`-on-prototype pattern as FARPROC above — unreferenced
// external-linkage prototype, no symbol emitted.
LIBC_MSABI void __pps_apc_routine_type_source(PVOID ApcArgument1,
                                              PVOID ApcArgument2,
                                              PVOID ApcArgument3);
using PPS_APC_ROUTINE = decltype(&__pps_apc_routine_type_source);

// Opaque; we never dereference — stored as PVOID in OBJECT_ATTRIBUTES.
using PSECURITY_DESCRIPTOR = PVOID;
using PSECURITY_QUALITY_OF_SERVICE = PVOID;

// UNICODE_STRING is now in <sys/ntabi.h>.

// STRING / ANSI_STRING — counted narrow string used by loader export lookups.
struct STRING {
  USHORT Length;        // Size in bytes, excluding NUL.
  USHORT MaximumLength; // Buffer capacity in bytes.
  PCHAR Buffer;
};
using PSTRING = STRING *;
using PCSTRING = const STRING *;
using ANSI_STRING = STRING;
using PANSI_STRING = ANSI_STRING *;
using PCANSI_STRING = const ANSI_STRING *;

// LdrLoadDll DllCharacteristics values used by KernelBase!LoadLibraryExW.
inline constexpr ULONG LDR_DONT_RESOLVE_DLL_REFERENCES = 0x00000002;
inline constexpr ULONG LDR_PACKAGED_LIBRARY = 0x00000004;
inline constexpr ULONG LDR_REQUIRE_SIGNED_TARGET = 0x00800000;
inline constexpr ULONG LDR_OS_INTEGRITY_CONTINUITY = 0x80000000;

// LdrLoadDll encodes search flags in the DllPath pointer when bit 0 is set.
inline constexpr ULONG LDR_PATH_IS_FLAGS = 0x00000001;
inline constexpr ULONG LDR_PATH_VALID_FLAGS = 0x00007F08;
inline constexpr ULONG LDR_PATH_WITH_ALTERED_SEARCH_PATH = 0x00000008;
inline constexpr ULONG LDR_PATH_SEARCH_DLL_LOAD_DIR = 0x00000100;
inline constexpr ULONG LDR_PATH_SEARCH_APPLICATION_DIR = 0x00000200;
inline constexpr ULONG LDR_PATH_SEARCH_USER_DIRS = 0x00000400;
inline constexpr ULONG LDR_PATH_SEARCH_SYSTEM32 = 0x00000800;
inline constexpr ULONG LDR_PATH_SEARCH_DEFAULT_DIRS = 0x00001000;
inline constexpr ULONG LDR_PATH_SAFE_CURRENT_DIRS = 0x00002000;
inline constexpr ULONG LDR_PATH_SEARCH_SYSTEM32_NO_FORWARDER = 0x00004000;

inline constexpr ULONG RTL_DOS_SEARCH_PATH_FLAG_APPLY_ISOLATION_REDIRECTION =
    0x00000001;
inline constexpr ULONG
    RTL_DOS_SEARCH_PATH_FLAG_DISALLOW_DOT_RELATIVE_PATH_SEARCH = 0x00000002;
inline constexpr ULONG
    RTL_DOS_SEARCH_PATH_FLAG_APPLY_DEFAULT_EXTENSION_WHEN_NOT_RELATIVE_PATH_EVEN_IF_FILE_HAS_EXTENSION =
        0x00000004;

inline PVOID LDR_MAPPEDVIEW_TO_DATAFILE(PVOID BaseAddress) {
  return reinterpret_cast<PVOID>(reinterpret_cast<ULONG_PTR>(BaseAddress) |
                                 static_cast<ULONG_PTR>(1));
}

inline PVOID LDR_MAPPEDVIEW_TO_IMAGEMAPPING(PVOID BaseAddress) {
  return reinterpret_cast<PVOID>(reinterpret_cast<ULONG_PTR>(BaseAddress) |
                                 static_cast<ULONG_PTR>(2));
}

// DllHandle low-bit queries — test whether a handle refers to a data file,
// image mapping, or either (resource). Inverse of the MAPPEDVIEW_TO_* helpers.
#define LDR_IS_DATAFILE(DllHandle) (((ULONG_PTR)(DllHandle)) & (ULONG_PTR)1)
#define LDR_IS_IMAGEMAPPING(DllHandle) (((ULONG_PTR)(DllHandle)) & (ULONG_PTR)2)
#define LDR_IS_RESOURCE(DllHandle)                                             \
  (LDR_IS_IMAGEMAPPING(DllHandle) || LDR_IS_DATAFILE(DllHandle))
#define LDR_DATAFILE_TO_MAPPEDVIEW(DllHandle)                                  \
  ((PVOID)(((ULONG_PTR)(DllHandle)) & ~(ULONG_PTR)1))
#define LDR_IMAGEMAPPING_TO_MAPPEDVIEW(DllHandle)                              \
  ((PVOID)(((ULONG_PTR)(DllHandle)) & ~(ULONG_PTR)2))

//===----------------------------------------------------------------------===//
// OBJECT_ATTRIBUTES — passed to NtCreateFile, NtOpenFile, NtDeleteFile, etc.
//===----------------------------------------------------------------------===//

inline constexpr ULONG OBJ_INHERIT = 0x00000002;
inline constexpr ULONG OBJ_CASE_INSENSITIVE = 0x00000040;
inline constexpr ULONG OBJ_OPENIF = 0x00000080;
inline constexpr ULONG OBJ_OPENLINK = 0x00000100;
inline constexpr ULONG OBJ_DONT_REPARSE = 0x00001000;

struct OBJECT_ATTRIBUTES {
  ULONG Length;
  HANDLE RootDirectory;
  PCUNICODE_STRING ObjectName;
  ULONG Attributes;
  PSECURITY_DESCRIPTOR SecurityDescriptor;
  PSECURITY_QUALITY_OF_SERVICE SecurityQualityOfService;
};
using POBJECT_ATTRIBUTES = OBJECT_ATTRIBUTES *;
using PCOBJECT_ATTRIBUTES = const OBJECT_ATTRIBUTES *;

// Signed priority value used by NT scheduler APIs.
using KPRIORITY = LONG;

// Process and thread identifier pair. Fields hold numeric IDs cast to HANDLE
// width — not real kernel handles and not closeable.
struct CLIENT_ID {
  HANDLE UniqueProcess; // PID
  HANDLE UniqueThread;  // TID
};
using PCLIENT_ID = CLIENT_ID *;

// Convenience initializer matching the SDK macro.
#define InitializeObjectAttributes(p, n, a, r, s)                              \
  {                                                                            \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES);                                   \
    (p)->RootDirectory = (r);                                                  \
    (p)->Attributes = (a);                                                     \
    (p)->ObjectName = (n);                                                     \
    (p)->SecurityDescriptor = (s);                                             \
    (p)->SecurityQualityOfService = nullptr;                                   \
  }

//===----------------------------------------------------------------------===//
// Fundamental Structures
//===----------------------------------------------------------------------===//

// Large integer for timeouts (100ns units) and file sizes
union LARGE_INTEGER {
  struct {
    ULONG LowPart;
    LONG HighPart;
  };
  long long QuadPart;
};

using PLARGE_INTEGER = LARGE_INTEGER *;

union ULARGE_INTEGER {
  struct {
    ULONG LowPart;
    ULONG HighPart;
  };
  unsigned long long QuadPart;
};

// File time (100ns intervals since January 1, 1601)
struct FILETIME {
  DWORD dwLowDateTime;
  DWORD dwHighDateTime;
};

// MEMORY_BASIC_INFORMATION - returned by
// NtQueryVirtualMemory(MemoryBasicInformation). Describes a contiguous region
// of pages with identical attributes. NtQueryVirtualMemory scans from
// BaseAddress upward until attributes change, then returns the region. Multiple
// regions may share the same AllocationBase (one reservation can contain
// sub-regions with different State/Protect).
struct MEMORY_BASIC_INFORMATION {
  // Base address of this region (page-aligned, rounded down from query
  // address).
  PVOID BaseAddress;
  // Base address of the containing allocation (the original
  // NtAllocateVirtualMemoryEx or NtMapViewOfSectionEx return value).
  // All sub-regions within one reservation share this value.
  PVOID AllocationBase;
  // PAGE_* protection at initial allocation time, or 0 if no access rights.
  DWORD AllocationProtect;
  WORD PartitionId; // Memory partition ID (Win10+)
  WORD Alignment;   // Padding for x64
  // Size in bytes of this contiguous region with identical attributes.
  SIZE_T RegionSize;
  // MEM_COMMIT, MEM_FREE, or MEM_RESERVE.
  // For MEM_FREE: AllocationBase, AllocationProtect, Protect, Type are
  // undefined. For MEM_RESERVE: Protect is undefined.
  DWORD State;
  // Current PAGE_* protection of the pages in this region.
  DWORD Protect;
  // MEM_IMAGE, MEM_MAPPED, or MEM_PRIVATE.
  DWORD Type;
};

//===----------------------------------------------------------------------===//
// I/O Status Block
//===----------------------------------------------------------------------===//

struct IO_STATUS_BLOCK {
  union {
    NTSTATUS Status;
    PVOID Pointer;
  };
  ULONG_PTR Information;
};
using PIO_STATUS_BLOCK = IO_STATUS_BLOCK *;

//===----------------------------------------------------------------------===//
// I/O APC Routine — callback for async NtReadFile/NtWriteFile completion
//===----------------------------------------------------------------------===//

LIBC_MSABI void __pio_apc_routine_type_source(PVOID ApcContext,
                                              IO_STATUS_BLOCK *IoStatusBlock,
                                              ULONG Reserved);
using PIO_APC_ROUTINE = decltype(&__pio_apc_routine_type_source);

//===----------------------------------------------------------------------===//
// Doubly-Linked List — used by PEB loader data and many kernel structures
//===----------------------------------------------------------------------===//

struct LIST_ENTRY {
  LIST_ENTRY *Flink;
  LIST_ENTRY *Blink;
};

// Singly-linked list node — used by LDR dependency tracking.
struct SINGLE_LIST_ENTRY {
  SINGLE_LIST_ENTRY *Next;
};
using PSINGLE_LIST_ENTRY = SINGLE_LIST_ENTRY *;

// Red-black tree node — used by LDR address/mapping index.
struct RTL_BALANCED_NODE {
  union {
    RTL_BALANCED_NODE *Children[2];
    struct {
      RTL_BALANCED_NODE *Left;
      RTL_BALANCED_NODE *Right;
    };
  };
  union {
    UCHAR Red : 1;
    UCHAR Balance : 2;
    ULONG_PTR ParentValue;
  };
};

// DLL entry point callback — DllMain signature. Invoked by the PE loader
// under MS x64 ABI. Same `decltype`-on-prototype pattern as FARPROC above.
LIBC_MSABI BOOLEAN __dll_init_routine_type_source(PVOID DllHandle,
                                                  ULONG Reason,
                                                  PVOID Context);
using PDLL_INIT_ROUTINE = decltype(&__dll_init_routine_type_source);

// Opaque loader context — used only as a pointer in LDR_DATA_TABLE_ENTRY.
typedef struct _LDRP_LOAD_CONTEXT LDRP_LOAD_CONTEXT, *PLDRP_LOAD_CONTEXT;

// Forward declaration — full definition in nt_peb.h.
typedef struct _ACTIVATION_CONTEXT ACTIVATION_CONTEXT, *PACTIVATION_CONTEXT;

typedef struct _LDR_SERVICE_TAG_RECORD {
  struct _LDR_SERVICE_TAG_RECORD *Next;
  ULONG ServiceTag;
} LDR_SERVICE_TAG_RECORD, *PLDR_SERVICE_TAG_RECORD;

// Loader dependency list (singly-linked with tail pointer).
typedef struct _LDRP_CSLIST {
  PSINGLE_LIST_ENTRY Tail;
} LDRP_CSLIST, *PLDRP_CSLIST;

// Loader dependency graph node state machine — tracks module loading progress.
typedef enum _LDR_DDAG_STATE {
  LdrModulesMerged = -5,
  LdrModulesInitError = -4,
  LdrModulesSnapError = -3,
  LdrModulesUnloaded = -2,
  LdrModulesUnloading = -1,
  LdrModulesPlaceHolder = 0,
  LdrModulesMapping = 1,
  LdrModulesMapped = 2,
  LdrModulesWaitingForDependencies = 3,
  LdrModulesSnapping = 4,
  LdrModulesSnapped = 5,
  LdrModulesCondensed = 6,
  LdrModulesReadyToInit = 7,
  LdrModulesInitializing = 8,
  LdrModulesReadyToRun = 9
} LDR_DDAG_STATE;

// Dependency graph node — one per loaded module, tracks load count and deps.
typedef struct _LDR_DDAG_NODE {
  LIST_ENTRY Modules;
  PLDR_SERVICE_TAG_RECORD ServiceTagList;
  ULONG LoadCount;
  ULONG LoadWhileUnloadingCount; // ReferenceCount before WIN10
  ULONG LowestLink;              // DependencyCount before WIN10
  union {
    LDRP_CSLIST Dependencies;
    SINGLE_LIST_ENTRY RemovalLink;
  };
  LDRP_CSLIST IncomingDependencies;
  LDR_DDAG_STATE State;
  SINGLE_LIST_ENTRY CondenseLink;
  ULONG PreorderNumber;
} LDR_DDAG_NODE, *PLDR_DDAG_NODE;

// Edge in the dependency graph linking parent and child DDAG nodes.
typedef struct _LDRP_DEPENDENCY {
  SINGLE_LIST_ENTRY Link;
  PLDR_DDAG_NODE ChildNode;
  SINGLE_LIST_ENTRY BackLink;
  union {
    PLDR_DDAG_NODE ParentNode;
    struct {
      ULONG ForwarderLink : 1;
      ULONG SpareFlags : 2;
    };
  };
} LDRP_DEPENDENCY, *PLDRP_DEPENDENCY;

// Why the loader loaded this DLL (static import, dynamic, delay-load, etc.).
typedef enum _LDR_DLL_LOAD_REASON {
  LoadReasonUnknown = -1,
  LoadReasonStaticDependency = 0,
  LoadReasonStaticForwarderDependency = 1,
  LoadReasonDynamicForwarderDependency = 2,
  LoadReasonDelayloadDependency = 3,
  LoadReasonDynamicLoad = 4,
  LoadReasonAsImageLoad = 5,
  LoadReasonAsDataLoad = 6,
  LoadReasonEnclavePrimary = 7, // since REDSTONE3
  LoadReasonEnclaveDependency = 8,
  LoadReasonPatchImage = 9, // since WIN11
} LDR_DLL_LOAD_REASON,
    *PLDR_DLL_LOAD_REASON;

// Hot-patch application state for this module.
typedef enum _LDR_HOT_PATCH_STATE {
  LdrHotPatchBaseImage,
  LdrHotPatchNotApplied,
  LdrHotPatchAppliedReverse,
  LdrHotPatchAppliedForward,
  LdrHotPatchFailedToPatch,
  LdrHotPatchStateMax,
} LDR_HOT_PATCH_STATE,
    *PLDR_HOT_PATCH_STATE;

// LDR_DATA_TABLE_ENTRY->Flags
#define LDRP_PACKAGED_BINARY 0x00000001
#define LDRP_MARKED_FOR_REMOVAL 0x00000002
#define LDRP_IMAGE_DLL 0x00000004
#define LDRP_LOAD_NOTIFICATIONS_SENT 0x00000008
#define LDRP_TELEMETRY_ENTRY_PROCESSED 0x00000010
#define LDRP_PROCESS_STATIC_IMPORT 0x00000020
#define LDRP_IN_LEGACY_LISTS 0x00000040
#define LDRP_IN_INDEXES 0x00000080
#define LDRP_SHIM_DLL 0x00000100
#define LDRP_IN_EXCEPTION_TABLE 0x00000200
#define LDRP_VERIFIER_PROVIDER 0x00000400        // reserved before WIN11 24H2
#define LDRP_SHIM_ENGINE_CALLOUT_SENT 0x00000800 // reserved before WIN11 24H2
#define LDRP_LOAD_IN_PROGRESS 0x00001000
#define LDRP_LOAD_CONFIG_PROCESSED 0x00002000 // reserved before WIN10
#define LDRP_ENTRY_PROCESSED 0x00004000
#define LDRP_PROTECT_DELAY_LOAD 0x00008000   // reserved before WINBLUE
#define LDRP_AUX_IAT_COPY_PRIVATE 0x00010000 // reserved before WIN11 24H2
#define LDRP_DONT_CALL_FOR_THREADS 0x00040000
#define LDRP_PROCESS_ATTACH_CALLED 0x00080000
#define LDRP_PROCESS_ATTACH_FAILED 0x00100000
#define LDRP_SCP_IN_EXCEPTION_TABLE                                            \
  0x00200000 // LDRP_COR_DEFERRED_VALIDATE before WIN11 24H2
#define LDRP_COR_IMAGE 0x00400000
#define LDRP_DONT_RELOCATE 0x00800000
#define LDRP_COR_IL_ONLY 0x01000000
#define LDRP_CHPE_IMAGE 0x02000000          // reserved before REDSTONE4
#define LDRP_CHPE_EMULATOR_IMAGE 0x04000000 // reserved before WIN11
#define LDRP_REDIRECTED 0x10000000
#define LDRP_COMPAT_DATABASE_PROCESSED 0x80000000

#define LDR_DATA_TABLE_ENTRY_SIZE_WINXP                                        \
  FIELD_OFFSET(LDR_DATA_TABLE_ENTRY, DdagNode)
#define LDR_DATA_TABLE_ENTRY_SIZE_WIN7                                         \
  FIELD_OFFSET(LDR_DATA_TABLE_ENTRY, BaseNameHashValue)
#define LDR_DATA_TABLE_ENTRY_SIZE_WIN8                                         \
  FIELD_OFFSET(LDR_DATA_TABLE_ENTRY, ImplicitPathOptions)
#define LDR_DATA_TABLE_ENTRY_SIZE_WIN10                                        \
  FIELD_OFFSET(LDR_DATA_TABLE_ENTRY, SigningLevel)
#define LDR_DATA_TABLE_ENTRY_SIZE_WIN11 sizeof(LDR_DATA_TABLE_ENTRY)

// Per-module loader table entry — one for each loaded DLL/EXE in the process.
typedef struct _LDR_DATA_TABLE_ENTRY {
  LIST_ENTRY InLoadOrderLinks;
  LIST_ENTRY InMemoryOrderLinks;
  LIST_ENTRY InInitializationOrderLinks;
  PVOID DllBase;
  PVOID EntryPoint; // PDLL_INIT_ROUTINE
  ULONG SizeOfImage;
  UNICODE_STRING FullDllName;
  UNICODE_STRING BaseDllName;
  union {
    UCHAR FlagGroup[4];
    ULONG Flags;
    struct {
      ULONG PackagedBinary : 1;
      ULONG MarkedForRemoval : 1;
      ULONG ImageDll : 1;
      ULONG LoadNotificationsSent : 1;
      ULONG TelemetryEntryProcessed : 1;
      ULONG ProcessStaticImport : 1;
      ULONG InLegacyLists : 1;
      ULONG InIndexes : 1;
      ULONG ShimDll : 1;
      ULONG InExceptionTable : 1;
      ULONG VerifierProvider : 1;      // 24H2
      ULONG ShimEngineCalloutSent : 1; // 24H2
      ULONG LoadInProgress : 1;
      ULONG LoadConfigProcessed : 1; // WIN10
      ULONG EntryProcessed : 1;
      ULONG ProtectDelayLoad : 1;  // WINBLUE
      ULONG AuxIatCopyPrivate : 1; // 24H2
      ULONG ReservedFlags3 : 1;
      ULONG DontCallForThreads : 1;
      ULONG ProcessAttachCalled : 1;
      ULONG ProcessAttachFailed : 1;
      ULONG ScpInExceptionTable : 1; // CorDeferredValidate before 24H2
      ULONG CorImage : 1;
      ULONG DontRelocate : 1;
      ULONG CorILOnly : 1;
      ULONG ChpeImage : 1;         // RS4
      ULONG ChpeEmulatorImage : 1; // WIN11
      ULONG ReservedFlags5 : 1;
      ULONG Redirected : 1;
      ULONG ReservedFlags6 : 2;
      ULONG CompatDatabaseProcessed : 1;
    };
  };
  USHORT ObsoleteLoadCount;
  USHORT TlsIndex;
  LIST_ENTRY HashLinks;
  ULONG TimeDateStamp;
  PACTIVATION_CONTEXT EntryPointActivationContext;
  PVOID Lock; // RtlAcquireSRWLockExclusive
  PLDR_DDAG_NODE DdagNode;
  LIST_ENTRY NodeModuleLink;
  PLDRP_LOAD_CONTEXT LoadContext;
  PVOID ParentDllBase;
  PVOID SwitchBackContext;
  RTL_BALANCED_NODE BaseAddressIndexNode;
  RTL_BALANCED_NODE MappingInfoIndexNode;
  PVOID OriginalBase;
  LARGE_INTEGER LoadTime;
  ULONG BaseNameHashValue;
  LDR_DLL_LOAD_REASON LoadReason;
  ULONG ImplicitPathOptions; // since WINBLUE
  ULONG ReferenceCount;      // since WIN10
  ULONG DependentLoadFlags;  // since RS1
  UCHAR SigningLevel;        // since RS2
  ULONG CheckSum;            // since WIN11
  PVOID ActivePatchImageBase;
  LDR_HOT_PATCH_STATE HotPatchState;
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

typedef const LDR_DATA_TABLE_ENTRY *PCLDR_DATA_TABLE_ENTRY;

//===----------------------------------------------------------------------===//
// PE Image Structures
//===----------------------------------------------------------------------===//

inline constexpr USHORT IMAGE_DOS_SIGNATURE = 0x5A4D; // "MZ"

struct IMAGE_DOS_HEADER {
  USHORT e_magic;
  USHORT e_cblp;
  USHORT e_cp;
  USHORT e_crlc;
  USHORT e_cparhdr;
  USHORT e_minalloc;
  USHORT e_maxalloc;
  USHORT e_ss;
  USHORT e_sp;
  USHORT e_csum;
  USHORT e_ip;
  USHORT e_cs;
  USHORT e_lfarlc;
  USHORT e_ovno;
  USHORT e_res[4];
  USHORT e_oemid;
  USHORT e_oeminfo;
  USHORT e_res2[10];
  LONG e_lfanew; // Offset to PE signature.
};

struct IMAGE_DATA_DIRECTORY {
  ULONG VirtualAddress;
  ULONG Size;
};

inline constexpr int IMAGE_NUMBEROF_DIRECTORY_ENTRIES = 16;
inline constexpr int IMAGE_DIRECTORY_ENTRY_EXPORT = 0;
inline constexpr int IMAGE_DIRECTORY_ENTRY_IMPORT = 1;
inline constexpr int IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3;
inline constexpr int IMAGE_DIRECTORY_ENTRY_TLS = 9;
inline constexpr int IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG = 10;

struct IMAGE_FILE_HEADER {
  USHORT Machine;
  USHORT NumberOfSections;
  ULONG TimeDateStamp;
  ULONG PointerToSymbolTable;
  ULONG NumberOfSymbols;
  USHORT SizeOfOptionalHeader;
  USHORT Characteristics;
};

struct IMAGE_OPTIONAL_HEADER64 {
  USHORT Magic;
  UCHAR MajorLinkerVersion;
  UCHAR MinorLinkerVersion;
  ULONG SizeOfCode;
  ULONG SizeOfInitializedData;
  ULONG SizeOfUninitializedData;
  ULONG AddressOfEntryPoint;
  ULONG BaseOfCode;
  ULONGLONG ImageBase;
  ULONG SectionAlignment;
  ULONG FileAlignment;
  USHORT MajorOperatingSystemVersion;
  USHORT MinorOperatingSystemVersion;
  USHORT MajorImageVersion;
  USHORT MinorImageVersion;
  USHORT MajorSubsystemVersion;
  USHORT MinorSubsystemVersion;
  ULONG Win32VersionValue;
  ULONG SizeOfImage;
  ULONG SizeOfHeaders;
  ULONG CheckSum;
  USHORT Subsystem;
  USHORT DllCharacteristics;
  ULONGLONG SizeOfStackReserve;
  ULONGLONG SizeOfStackCommit;
  ULONGLONG SizeOfHeapReserve;
  ULONGLONG SizeOfHeapCommit;
  ULONG LoaderFlags;
  ULONG NumberOfRvaAndSizes;
  IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
};

inline constexpr ULONG IMAGE_NT_SIGNATURE = 0x00004550; // "PE\0\0"

struct IMAGE_NT_HEADERS64 {
  ULONG Signature;
  IMAGE_FILE_HEADER FileHeader;
  IMAGE_OPTIONAL_HEADER64 OptionalHeader;
};

// Section header table follows the OptionalHeader. Count is given by
// IMAGE_FILE_HEADER::NumberOfSections.
struct IMAGE_SECTION_HEADER {
  UCHAR Name[8];
  union {
    ULONG PhysicalAddress;
    ULONG VirtualSize;
  } Misc;
  ULONG VirtualAddress;
  ULONG SizeOfRawData;
  ULONG PointerToRawData;
  ULONG PointerToRelocations;
  ULONG PointerToLinenumbers;
  USHORT NumberOfRelocations;
  USHORT NumberOfLinenumbers;
  ULONG Characteristics;
};

// IMAGE_SECTION_HEADER::Characteristics bits (subset).
inline constexpr ULONG IMAGE_SCN_CNT_CODE = 0x00000020;
inline constexpr ULONG IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
inline constexpr ULONG IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
inline constexpr ULONG IMAGE_SCN_MEM_DISCARDABLE = 0x02000000;
inline constexpr ULONG IMAGE_SCN_MEM_NOT_CACHED = 0x04000000;
inline constexpr ULONG IMAGE_SCN_MEM_NOT_PAGED = 0x08000000;
inline constexpr ULONG IMAGE_SCN_MEM_SHARED = 0x10000000;
inline constexpr ULONG IMAGE_SCN_MEM_EXECUTE = 0x20000000;
inline constexpr ULONG IMAGE_SCN_MEM_READ = 0x40000000;
inline constexpr ULONG IMAGE_SCN_MEM_WRITE = 0x80000000;

struct IMAGE_EXPORT_DIRECTORY {
  ULONG Characteristics;
  ULONG TimeDateStamp;
  USHORT MajorVersion;
  USHORT MinorVersion;
  ULONG Name;
  ULONG Base;
  ULONG NumberOfFunctions;
  ULONG NumberOfNames;
  ULONG AddressOfFunctions;    // RVA → ULONG[] of function RVAs.
  ULONG AddressOfNames;        // RVA → ULONG[] of name string RVAs.
  ULONG AddressOfNameOrdinals; // RVA → USHORT[] mapping name index → ordinal.
};

struct IMAGE_IMPORT_DESCRIPTOR {
  union {
    ULONG Characteristics;
    ULONG OriginalFirstThunk; // RVA to Import Lookup Table (ILT).
  };
  ULONG TimeDateStamp;
  ULONG ForwarderChain;
  ULONG Name;                  // RVA to DLL name string.
  ULONG FirstThunk;            // RVA to Import Address Table (IAT).
};

struct IMAGE_TLS_DIRECTORY64 {
  ULONGLONG StartAddressOfRawData;
  ULONGLONG EndAddressOfRawData;
  ULONGLONG AddressOfIndex;
  ULONGLONG AddressOfCallBacks;
  ULONG SizeOfZeroFill;
  ULONG Characteristics;
};

// TLS callback function pointer type. Invoked by the PE loader under MS
// x64 ABI. Same `decltype`-on-prototype pattern as FARPROC above.
LIBC_MSABI void __image_tls_callback_type_source(PVOID DllHandle,
                                                 ULONG Reason,
                                                 PVOID Reserved);
using PIMAGE_TLS_CALLBACK = decltype(&__image_tls_callback_type_source);

// DLL notification reasons (used by TLS callbacks and DllMain).
inline constexpr ULONG DLL_PROCESS_DETACH = 0;
inline constexpr ULONG DLL_PROCESS_ATTACH = 1;
inline constexpr ULONG DLL_THREAD_ATTACH = 2;
inline constexpr ULONG DLL_THREAD_DETACH = 3;

// Function override relocation types in DVRT records.

#define IMAGE_FUNCTION_OVERRIDE_INVALID         0
#define IMAGE_FUNCTION_OVERRIDE_X64_REL32       1  // 32-bit relative address from byte following reloc
#define IMAGE_FUNCTION_OVERRIDE_ARM64_BRANCH26  2  // 26 bit offset << 2 & sign ext. for B & BL
#define IMAGE_FUNCTION_OVERRIDE_ARM64_THUNK     3

//
// Code Integrity in loadconfig (CI)
//

typedef struct _IMAGE_LOAD_CONFIG_CODE_INTEGRITY {
    WORD    Flags;          // Flags to indicate if CI information is available, etc.
    WORD    Catalog;        // 0xFFFF means not available
    DWORD   CatalogOffset;
    DWORD   Reserved;       // Additional bitmask to be defined later
} IMAGE_LOAD_CONFIG_CODE_INTEGRITY, *PIMAGE_LOAD_CONFIG_CODE_INTEGRITY;

//
// Dynamic value relocation table in loadconfig
//

typedef struct _IMAGE_DYNAMIC_RELOCATION_TABLE {
    DWORD Version;
    DWORD Size;
//  IMAGE_DYNAMIC_RELOCATION DynamicRelocations[0];
} IMAGE_DYNAMIC_RELOCATION_TABLE, *PIMAGE_DYNAMIC_RELOCATION_TABLE;


// IMAGE_LOAD_CONFIG_DIRECTORY64 — partial definition covering fields through
// the CFG (Control Flow Guard) section. The full struct grows with each Windows
// release; we only define fields up to GuardLongJumpTargetCount (offset 0x98).
// Access is always bounds-checked via DataDirectory[10].Size.
typedef struct _IMAGE_LOAD_CONFIG_DIRECTORY64 {
    DWORD      Size;
    DWORD      TimeDateStamp;
    WORD       MajorVersion;
    WORD       MinorVersion;
    DWORD      GlobalFlagsClear;
    DWORD      GlobalFlagsSet;
    DWORD      CriticalSectionDefaultTimeout;
    ULONGLONG  DeCommitFreeBlockThreshold;
    ULONGLONG  DeCommitTotalFreeThreshold;
    ULONGLONG  LockPrefixTable;                // VA
    ULONGLONG  MaximumAllocationSize;
    ULONGLONG  VirtualMemoryThreshold;
    ULONGLONG  ProcessAffinityMask;
    DWORD      ProcessHeapFlags;
    WORD       CSDVersion;
    WORD       DependentLoadFlags;
    ULONGLONG  EditList;                       // VA
    ULONGLONG  SecurityCookie;                 // VA
    ULONGLONG  SEHandlerTable;                 // VA
    ULONGLONG  SEHandlerCount;
    ULONGLONG  GuardCFCheckFunctionPointer;    // VA
    ULONGLONG  GuardCFDispatchFunctionPointer; // VA
    ULONGLONG  GuardCFFunctionTable;           // VA
    ULONGLONG  GuardCFFunctionCount;
    DWORD      GuardFlags;
    IMAGE_LOAD_CONFIG_CODE_INTEGRITY CodeIntegrity;
    ULONGLONG  GuardAddressTakenIatEntryTable; // VA
    ULONGLONG  GuardAddressTakenIatEntryCount;
    ULONGLONG  GuardLongJumpTargetTable;       // VA
    ULONGLONG  GuardLongJumpTargetCount;
    ULONGLONG  DynamicValueRelocTable;         // VA
    ULONGLONG  CHPEMetadataPointer;            // VA
    ULONGLONG  GuardRFFailureRoutine;          // VA
    ULONGLONG  GuardRFFailureRoutineFunctionPointer; // VA
    DWORD      DynamicValueRelocTableOffset;
    WORD       DynamicValueRelocTableSection;
    WORD       Reserved2;
    ULONGLONG  GuardRFVerifyStackPointerFunctionPointer; // VA
    DWORD      HotPatchTableOffset;
    DWORD      Reserved3;
    ULONGLONG  EnclaveConfigurationPointer;    // VA
    ULONGLONG  VolatileMetadataPointer;        // VA
    ULONGLONG  GuardEHContinuationTable;       // VA
    ULONGLONG  GuardEHContinuationCount;
    ULONGLONG  GuardXFGCheckFunctionPointer;   // VA
    ULONGLONG  GuardXFGDispatchFunctionPointer; // VA
    ULONGLONG  GuardXFGTableDispatchFunctionPointer; // VA
    ULONGLONG  CastGuardOsDeterminedFailureMode; // VA
    ULONGLONG  GuardMemcpyFunctionPointer;     // VA
    ULONGLONG  UmaFunctionPointers;            // VA
} IMAGE_LOAD_CONFIG_DIRECTORY64, *PIMAGE_LOAD_CONFIG_DIRECTORY64;

// DllCharacteristics flag for Control Flow Guard.
inline constexpr USHORT IMAGE_DLLCHARACTERISTICS_GUARD_CF = 0x4000;

// GuardFlags values.
inline constexpr ULONG IMAGE_GUARD_CF_INSTRUMENTED = 0x00000100;
inline constexpr ULONG IMAGE_GUARD_CFW_INSTRUMENTED = 0x00000200;
inline constexpr ULONG IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT = 0x00000400;

// RUNTIME_FUNCTION is already defined in <sys/ntabi.h>.

// Machine type constants.
inline constexpr USHORT IMAGE_FILE_MACHINE_AMD64 = 0x8664;
inline constexpr USHORT IMAGE_FILE_MACHINE_ARM64 = 0xAA64;

// Subsystem constants.
inline constexpr USHORT IMAGE_SUBSYSTEM_NATIVE = 1;
inline constexpr USHORT IMAGE_SUBSYSTEM_WINDOWS_GUI = 2;
inline constexpr USHORT IMAGE_SUBSYSTEM_WINDOWS_CUI = 3;

// ABI layout validation for libc-internal NT types.
// PE image structures — must match PE/COFF spec exactly.
static_assert(sizeof(IMAGE_DOS_HEADER) == 64,
              "IMAGE_DOS_HEADER must be 64 bytes");
static_assert(FIELD_OFFSET(IMAGE_DOS_HEADER, e_lfanew) == 0x3C,
              "IMAGE_DOS_HEADER::e_lfanew must be at offset 0x3C");
static_assert(sizeof(IMAGE_DATA_DIRECTORY) == 8,
              "IMAGE_DATA_DIRECTORY must be 8 bytes");
static_assert(sizeof(IMAGE_FILE_HEADER) == 20,
              "IMAGE_FILE_HEADER must be 20 bytes");
static_assert(sizeof(IMAGE_OPTIONAL_HEADER64) == 240,
              "IMAGE_OPTIONAL_HEADER64 must be 240 bytes");
static_assert(FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, AddressOfEntryPoint) == 16,
              "IMAGE_OPTIONAL_HEADER64::AddressOfEntryPoint at offset 16");
static_assert(FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, ImageBase) == 24,
              "IMAGE_OPTIONAL_HEADER64::ImageBase at offset 24");
static_assert(FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, DataDirectory) == 112,
              "IMAGE_OPTIONAL_HEADER64::DataDirectory at offset 0x70");
static_assert(sizeof(IMAGE_NT_HEADERS64) == 264,
              "IMAGE_NT_HEADERS64 must be 264 bytes");
static_assert(sizeof(IMAGE_SECTION_HEADER) == 40,
              "IMAGE_SECTION_HEADER must be 40 bytes");
static_assert(FIELD_OFFSET(IMAGE_SECTION_HEADER, Characteristics) == 36,
              "IMAGE_SECTION_HEADER::Characteristics must be at offset 36");
static_assert(sizeof(IMAGE_EXPORT_DIRECTORY) == 40,
              "IMAGE_EXPORT_DIRECTORY must be 40 bytes");
static_assert(sizeof(IMAGE_IMPORT_DESCRIPTOR) == 20,
              "IMAGE_IMPORT_DESCRIPTOR must be 20 bytes");
static_assert(FIELD_OFFSET(IMAGE_IMPORT_DESCRIPTOR, Name) == 12,
              "IMAGE_IMPORT_DESCRIPTOR::Name must be at offset 12");
static_assert(FIELD_OFFSET(IMAGE_IMPORT_DESCRIPTOR, FirstThunk) == 16,
              "IMAGE_IMPORT_DESCRIPTOR::FirstThunk must be at offset 16");
static_assert(sizeof(IMAGE_TLS_DIRECTORY64) == 40,
              "IMAGE_TLS_DIRECTORY64 must be 40 bytes");
// IMAGE_LOAD_CONFIG_DIRECTORY64: validate CFG field offsets (partial struct).
static_assert(FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64,
                           GuardCFCheckFunctionPointer) == 0x70,
              "IMAGE_LOAD_CONFIG_DIRECTORY64::GuardCFCheckFunctionPointer "
              "must be at offset 0x70");
static_assert(FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64,
                           GuardCFDispatchFunctionPointer) == 0x78,
              "IMAGE_LOAD_CONFIG_DIRECTORY64::GuardCFDispatchFunctionPointer "
              "must be at offset 0x78");
static_assert(FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64,
                           GuardCFFunctionTable) == 0x80,
              "IMAGE_LOAD_CONFIG_DIRECTORY64::GuardCFFunctionTable "
              "must be at offset 0x80");
static_assert(FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64,
                           GuardCFFunctionCount) == 0x88,
              "IMAGE_LOAD_CONFIG_DIRECTORY64::GuardCFFunctionCount "
              "must be at offset 0x88");
static_assert(FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags) == 0x90,
              "IMAGE_LOAD_CONFIG_DIRECTORY64::GuardFlags "
              "must be at offset 0x90");
// LARGE_INTEGER must be exactly 8 bytes so it aliases long long cleanly.
static_assert(sizeof(LARGE_INTEGER) == 8, "LARGE_INTEGER must be 8 bytes");
// CLIENT_ID holds a PID and TID each cast to HANDLE width.
static_assert(sizeof(CLIENT_ID) == 16, "CLIENT_ID must be 16 bytes");
// OBJECT_ATTRIBUTES: ULONG Length + 4 pad then five pointer-width fields.
static_assert(sizeof(OBJECT_ATTRIBUTES) == 48,
              "OBJECT_ATTRIBUTES must be 48 bytes");
static_assert(__builtin_offsetof(OBJECT_ATTRIBUTES, RootDirectory) == 8,
              "OBJECT_ATTRIBUTES::RootDirectory must be at offset 8");
static_assert(__builtin_offsetof(OBJECT_ATTRIBUTES, SecurityDescriptor) == 32,
              "OBJECT_ATTRIBUTES::SecurityDescriptor must be at offset 32");
// MEMORY_BASIC_INFORMATION: kernel writes this directly; any packing mismatch
// corrupts every field after AllocationBase.
static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 48,
              "MEMORY_BASIC_INFORMATION must be 48 bytes");
static_assert(
    __builtin_offsetof(MEMORY_BASIC_INFORMATION, AllocationProtect) == 16,
    "MEMORY_BASIC_INFORMATION::AllocationProtect must be at offset 16");
static_assert(__builtin_offsetof(MEMORY_BASIC_INFORMATION, RegionSize) == 24,
              "MEMORY_BASIC_INFORMATION::RegionSize must be at offset 24");
static_assert(__builtin_offsetof(MEMORY_BASIC_INFORMATION, State) == 32,
              "MEMORY_BASIC_INFORMATION::State must be at offset 32");
// IO_STATUS_BLOCK: the union expands to PVOID width; Information follows.
static_assert(sizeof(IO_STATUS_BLOCK) == 16,
              "IO_STATUS_BLOCK must be 16 bytes");
static_assert(__builtin_offsetof(IO_STATUS_BLOCK, Information) == 8,
              "IO_STATUS_BLOCK::Information must be at offset 8");

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_TYPES_H
