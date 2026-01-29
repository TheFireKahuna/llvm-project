//===-- crt_windows_internal.h - CRT internal definitions ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared types, imports, and macros for Windows CRT implementation.
//
// C++ CONSTRAINTS: This code runs before the C++ runtime is initialized.
// - NO exceptions (throw, try/catch)
// - NO RTTI (dynamic_cast, typeid)
// - NO heap (new/delete) - we ARE the memory init code
// - NO static constructors - we ARE the constructor runner
// - NO std:: anything
//
// ALLOWED: extern "C", templates, constexpr, enum class, nullptr,
//          static_cast, reinterpret_cast, inline functions
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_CRT_CRT_WINDOWS_INTERNAL_H
#define COMPILER_RT_LIB_CRT_CRT_WINDOWS_INTERNAL_H

#ifndef _WIN32
#error "This file is Windows-only"
#endif

#include <stddef.h>
#include <stdint.h>

//===----------------------------------------------------------------------===//
// Architecture detection
//===----------------------------------------------------------------------===//

#if defined(_M_ARM64EC)
// ARM64EC: ARM64 code that interops with x64 using x64 calling conventions.
#define CRT_ARCH_ARM64EC 1
#define CRT_ARCH_ARM64 1
#define CRT_ARCH_NAME "arm64ec"
#elif defined(_M_X64) || defined(__x86_64__)
#define CRT_ARCH_X64 1
#define CRT_ARCH_NAME "x86_64"
#elif defined(_M_IX86) || defined(__i386__)
#define CRT_ARCH_X86 1
#define CRT_ARCH_NAME "i386"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define CRT_ARCH_ARM64 1
#define CRT_ARCH_NAME "aarch64"
#elif defined(_M_ARM) || defined(__arm__)
#define CRT_ARCH_ARM 1
#define CRT_ARCH_NAME "arm"
#else
#error "Unsupported architecture for Windows CRT"
#endif

// Symbol prefix for x86 (underscore decoration)
#if defined(CRT_ARCH_X86)
#define CRT_SYM_PREFIX "_"
#else
#define CRT_SYM_PREFIX ""
#endif

// Architecture validation - catch miscompilation early
#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
static_assert(sizeof(void*) == 8,
              "64-bit architecture detected but pointer size is not 8 bytes");
#elif defined(CRT_ARCH_X86) || defined(CRT_ARCH_ARM)
static_assert(sizeof(void*) == 4,
              "32-bit architecture detected but pointer size is not 4 bytes");
#endif

//===----------------------------------------------------------------------===//
// Compiler attributes
//===----------------------------------------------------------------------===//

#if defined(__clang__) || defined(__GNUC__)
#define CRT_HIDDEN __attribute__((visibility("hidden")))
#define CRT_NOINLINE __attribute__((noinline))
#define CRT_NORETURN __attribute__((noreturn))
#define CRT_USED __attribute__((used))
#define CRT_WEAK __attribute__((weak))
#define CRT_ALIGNED(n) __attribute__((aligned(n)))
#else
#define CRT_HIDDEN
#define CRT_NOINLINE __declspec(noinline)
#define CRT_NORETURN __declspec(noreturn)
#define CRT_USED
#define CRT_WEAK
#define CRT_ALIGNED(n) __declspec(align(n))
#endif

// Export macro for public CRT API functions.
// On Windows, public functions use __cdecl calling convention.
// This matches MSVC's CRT exports and ensures ABI compatibility.
// Note: Defined outside extern "C" blocks to avoid unusual macro context.
#define CRT_API __cdecl

//===----------------------------------------------------------------------===//
// Exit codes and UCRT runtime error codes
//===----------------------------------------------------------------------===//

namespace crt {

enum class ExitCode : unsigned {
  Success = 0,
  Purecall = 3,
  AllocFailure = 254,
  InitFailure = 255
};

// Implicit conversion to unsigned for ExitProcess
inline unsigned toUnsigned(ExitCode code) {
  return static_cast<unsigned>(code);
}

} // namespace crt

// Legacy macros for C-style code
#define CRT_EXIT_SUCCESS 0
#define CRT_EXIT_PURECALL 3
#define CRT_EXIT_ALLOC_FAILURE 254
#define CRT_EXIT_INIT_FAILURE 255

// UCRT runtime error codes for _amsg_exit()
// These match the _RT_* constants from MSVC's crtdefs.h
namespace crt {

enum class RuntimeError : int {
  // Memory errors
  SpaceArg = 8,      // _RT_SPACEARG - not enough space for arguments
  SpaceEnv = 9,      // _RT_SPACEENV - not enough space for environment
  Heap = 28,         // _RT_HEAP - heap error

  // Initialization errors
  CrtInit = 27,      // _RT_CRNL - CRT not initialized
  Banner = 24,       // _RT_BANNER - abnormal program termination

  // Internal errors
  InternalErr = 30,  // Internal CRT error
  PseudoReloc = 31,  // Pseudo-relocation error (custom)
};

} // namespace crt

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

namespace crt {

// Floating-point support magic value.
// Historical MSVC value - the linker looks for _fltused when floating-point
// code is present. The specific value 0x9875 has been used since early MSVC
// versions and is maintained for compatibility.
constexpr int FltusedMagic = 0x9875;

// Cache line size for alignment
constexpr size_t CacheLineSize = 64;

// Fast fail codes
constexpr unsigned FastFailStackCookieCheckFailure = 2;

// Security cookie defaults
#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
using CookieType = uint64_t;
constexpr CookieType DefaultSecurityCookie = 0x00002B992DDFA232ULL;
#else
using CookieType = uint32_t;
constexpr CookieType DefaultSecurityCookie = 0xBB40E64EUL;
#endif

// Validate CookieType matches pointer size (security cookie should be
// pointer-sized to properly protect return addresses on the stack)
static_assert(sizeof(CookieType) == sizeof(void*),
              "CookieType size must match pointer size");

} // namespace crt

//===----------------------------------------------------------------------===//
// Windows type definitions - SDK compatible
//
// These are guarded to prevent conflicts when <windows.h> is included.
// We check for _WINDEF_ which is defined by windef.h (included by windows.h).
//===----------------------------------------------------------------------===//

#ifndef _WINDEF_
using BOOL = int;
using BYTE = unsigned char;
using WORD = unsigned short;
using DWORD = unsigned long;
using ULONG = unsigned long;
using DWORD64 = unsigned __int64;
using HANDLE = void*;
using HINSTANCE = void*;
using HMODULE = void*;
using LPVOID = void*;
using LPCVOID = const void*;
using LPSTR = char*;
using LPCSTR = const char*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using NTSTATUS = long;
#endif // _WINDEF_

// STARTUPINFO structure for GetStartupInfoW
// Guard against winbase.h which defines this
#ifndef _WINBASE_
struct STARTUPINFOW {
  DWORD cb;
  LPWSTR lpReserved;
  LPWSTR lpDesktop;
  LPWSTR lpTitle;
  DWORD dwX;
  DWORD dwY;
  DWORD dwXSize;
  DWORD dwYSize;
  DWORD dwXCountChars;
  DWORD dwYCountChars;
  DWORD dwFillAttribute;
  DWORD dwFlags;
  WORD wShowWindow;
  WORD cbReserved2;
  void* lpReserved2;
  HANDLE hStdInput;
  HANDLE hStdOutput;
  HANDLE hStdError;
};
#endif // _WINBASE_

// INIT_ONCE for thread-safe one-time initialization (Vista+)
// Guard against synchapi.h
#ifndef _SYNCHAPI_H_
union INIT_ONCE {
  void* Ptr;
};
#define INIT_ONCE_STATIC_INIT {nullptr}
#endif // _SYNCHAPI_H_

// Callback type for InitOnceExecuteOnce
using PINIT_ONCE = INIT_ONCE*;
using INIT_ONCE_FN = BOOL(__stdcall*)(PINIT_ONCE, void*, void**);

// LARGE_INTEGER for QueryPerformanceCounter
// Guard against winnt.h
#ifndef _WINNT_
union LARGE_INTEGER {
  struct {
    DWORD LowPart;
    long HighPart;
  };
  __int64 QuadPart;
};
#endif // _WINNT_

// FILETIME helper
union crt_filetime_t {
  unsigned __int64 scalar;
  struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
  } ft;
};

//===----------------------------------------------------------------------===//
// Windows constants (in crt namespace to avoid conflicts with <windows.h>)
//
// These are namespaced to prevent ODR violations if user code includes
// <windows.h>. Use crt::kStartfUseshowwindow instead of STARTF_USESHOWWINDOW.
//===----------------------------------------------------------------------===//

namespace crt {

// Startup info flags
constexpr DWORD kStartfUseshowwindow = 0x00000001;

// Show window commands
constexpr int kSwShowdefault = 10;

// Heap allocation flags
constexpr DWORD kHeapZeroMemory = 0x00000008;

// Memory protection flags (for VirtualProtect)
constexpr DWORD kPageReadonly = 0x02;
constexpr DWORD kPageReadwrite = 0x04;
constexpr DWORD kPageExecute = 0x10;
constexpr DWORD kPageExecuteRead = 0x20;
constexpr DWORD kPageExecuteReadwrite = 0x40;

// DLL reason codes
constexpr DWORD kDllProcessAttach = 1;
constexpr DWORD kDllThreadAttach = 2;
constexpr DWORD kDllThreadDetach = 3;
constexpr DWORD kDllProcessDetach = 0;

} // namespace crt

//===----------------------------------------------------------------------===//
// Function pointer types
//===----------------------------------------------------------------------===//

// CRT initializers
using crt_pifv_t = int (*)(void);   // C initializers (return int for error)
using crt_pvfv_t = void (*)(void);  // C++ constructors (return void)

// Legacy MSVC names
using _PIFV = crt_pifv_t;
using _PVFV = crt_pvfv_t;

// Handler types
using _purecall_handler = int(__cdecl*)(void);
using _invalid_parameter_handler = void(__cdecl*)(
    const wchar_t* expression,
    const wchar_t* function,
    const wchar_t* file,
    unsigned int line,
    uintptr_t reserved);

// TLS callback type
using PIMAGE_TLS_CALLBACK = void(__stdcall*)(void* DllHandle, DWORD Reason,
                                              void* Reserved);

//===----------------------------------------------------------------------===//
// kernel32.dll imports
//===----------------------------------------------------------------------===//

extern "C" {
__declspec(dllimport) BOOL __stdcall DisableThreadLibraryCalls(HINSTANCE);
__declspec(dllimport) CRT_NORETURN void __stdcall ExitProcess(unsigned);
__declspec(dllimport) LPSTR __stdcall GetCommandLineA(void);
__declspec(dllimport) LPWSTR __stdcall GetCommandLineW(void);
__declspec(dllimport) LPSTR __stdcall GetEnvironmentStringsA(void);
__declspec(dllimport) LPWSTR __stdcall GetEnvironmentStringsW(void);
__declspec(dllimport) BOOL __stdcall FreeEnvironmentStringsA(LPSTR);
__declspec(dllimport) BOOL __stdcall FreeEnvironmentStringsW(LPWSTR);
__declspec(dllimport) HANDLE __stdcall GetProcessHeap(void);
__declspec(dllimport) LPVOID __stdcall HeapAlloc(HANDLE, DWORD, size_t);
__declspec(dllimport) LPVOID __stdcall HeapReAlloc(HANDLE, DWORD, LPVOID, size_t);
__declspec(dllimport) BOOL __stdcall HeapFree(HANDLE, DWORD, LPVOID);
__declspec(dllimport) void __stdcall GetStartupInfoW(STARTUPINFOW*);
__declspec(dllimport) HINSTANCE __stdcall GetModuleHandleW(LPCWSTR);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);
__declspec(dllimport) DWORD __stdcall GetCurrentThreadId(void);
__declspec(dllimport) BOOL __stdcall QueryPerformanceCounter(LARGE_INTEGER*);
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(crt_filetime_t*);
__declspec(dllimport) unsigned __int64 __stdcall GetTickCount64(void);
__declspec(dllimport) void __stdcall Sleep(DWORD);

// One-time initialization (Vista+)
// InitOnceExecuteOnce ensures a callback runs exactly once, even with
// concurrent callers. Waiting threads sleep efficiently (kernel-assisted).
__declspec(dllimport) BOOL __stdcall InitOnceExecuteOnce(
    PINIT_ONCE InitOnce, INIT_ONCE_FN InitFn, void* Parameter, void** Context);

// Debug output (for _amsg_exit diagnostics)
__declspec(dllimport) void __stdcall OutputDebugStringA(LPCSTR);

// Heap validation (for debug builds)
__declspec(dllimport) BOOL __stdcall HeapValidate(HANDLE, DWORD, LPCVOID);
}

//===----------------------------------------------------------------------===//
// UCRT imports and delegation
//
// DESIGN PRINCIPLE: Delegate to UCRT whenever possible.
//
// The Universal C Runtime (UCRT) provides well-tested, maintained
// implementations of standard C library functions. We import only what we
// need for CRT startup and delegate everything else to UCRT.
//
// FUNCTION OWNERSHIP SUMMARY:
// ==========================
//
// THIS CRT PROVIDES (not from UCRT):
// ----------------------------------
// Symbol                    | Purpose
// --------------------------|------------------------------------------
// mainCRTStartup, etc.      | Entry points (we ARE the entry)
// exit()                    | Calls __cxa_finalize, then UCRT's _exit()
// __security_cookie         | Stack protection (init timing critical)
// __security_init_cookie    | Cookie initialization
// __report_gsfailure        | Cookie mismatch handler
// _initterm, _initterm_e    | Section callback runners
// _purecall                 | Pure virtual call handler (bridges to Itanium)
// __dso_handle              | DSO identifier for __cxa_atexit
// _CRT_INIT                 | DLL init for custom loaders
// _cexit, _c_exit           | Cleanup without termination
// __argc, __argv, _environ  | Command line globals
// _tls_used, _tls_index     | TLS support structures
//
// UCRT PROVIDES (we delegate/import):
// -----------------------------------
// Symbol                    | Why UCRT owns it
// --------------------------|------------------------------------------
// abort()                   | We import and call it
// _exit()                   | We import and call it (from our exit())
// _amsg_exit()              | Runtime error termination with message
// atexit(), _onexit()       | Complex handler list management
// quick_exit(), at_quick_exit() | C11 quick exit mechanism
// errno, _errno             | Thread-local error storage
// _set_error_mode()         | Error dialog control
// _set_abort_behavior()     | Abort flags
// _set_new_mode()           | malloc/new failure behavior
// _invalid_parameter*       | Parameter validation infrastructure
// _controlfp_s, _control87  | FPU control word
// _configthreadlocale       | Locale management
// All stdio (printf, etc.)  | I/O subsystem
// All malloc/free           | Heap management
//
// COMES FROM libc++abi (optional):
// --------------------------------
// __cxa_atexit              | C++ destructor registration
// __cxa_finalize            | C++ destructor invocation
// __cxa_pure_virtual        | Itanium pure virtual handler
// __cxa_deleted_virtual     | Itanium deleted virtual handler
//
// PLATFORM NOTES:
//
// _CRT_ATFORK_LOCK / pthread_atfork:
//   Not applicable on Windows. The fork() system call doesn't exist in the
//   Windows process model. Windows uses CreateProcess() which creates a new
//   process with its own address space. No fork-related locking is needed.
//
// Side-by-Side Assemblies (SxS) / Manifest Handling:
//   Handled automatically by UCRT and the Windows loader. When your executable
//   or DLL has a manifest (embedded or external) requesting specific UCRT
//   versions, the loader handles assembly binding. This CRT implementation
//   is compatible with SxS - it delegates to whatever UCRT version is loaded.
//   No special manifest handling code is required here.
//
//===----------------------------------------------------------------------===//

extern "C" {
__declspec(dllimport) CRT_NORETURN void __cdecl abort(void);
__declspec(dllimport) CRT_NORETURN void __cdecl _exit(int);
__declspec(dllimport) CRT_NORETURN void __cdecl _amsg_exit(int);

// FPU control - used to initialize floating-point state
__declspec(dllimport) void __cdecl _fpreset(void);
}

//===----------------------------------------------------------------------===//
// Centralized fatal error handling
//
// All CRT fatal errors go through these functions for consistency.
// We use _amsg_exit() when possible (provides user-visible error message),
// falling back to __fastfail() for errors that occur before UCRT is ready.
//===----------------------------------------------------------------------===//

namespace crt {

/// Fatal error with UCRT error message display.
/// Use this for errors after UCRT is initialized (most allocation failures).
/// \param errCode One of the RuntimeError codes for _amsg_exit().
CRT_NORETURN inline void fatalError(RuntimeError errCode) {
  _amsg_exit(static_cast<int>(errCode));
}

/// Fatal error with debug output, using __fastfail for immediate termination.
/// Use this for errors that occur BEFORE UCRT is ready (pseudo-relocation,
/// very early init) or for security-critical failures.
/// \param msg Debug message output via OutputDebugStringA.
CRT_NORETURN inline void fatalErrorEarly(const char* msg) {
  OutputDebugStringA("FATAL: CRT initialization error: ");
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  // Use __fastfail for immediate termination with crash dump support.
  // This cannot be intercepted and ensures we don't run any more code.
  __fastfail(FastFailStackCookieCheckFailure);
}

/// Fatal error with both debug output and UCRT error display.
/// Use this when you have a specific message but also want _amsg_exit behavior.
CRT_NORETURN inline void fatalErrorWithMessage(const char* msg,
                                                RuntimeError errCode) {
  OutputDebugStringA("FATAL: ");
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  _amsg_exit(static_cast<int>(errCode));
}

} // namespace crt

//===----------------------------------------------------------------------===//
// Our exit() implementation (defined in crt_windows_init.cpp)
//
// We provide our own exit() that calls __cxa_finalize before delegating to
// UCRT's _exit(). This ensures C++ static destructors registered via
// __cxa_atexit are called in the correct order.
//===----------------------------------------------------------------------===//

extern "C" CRT_NORETURN void __cdecl exit(int);

//===----------------------------------------------------------------------===//
// libc++abi imports (weak symbols)
//===----------------------------------------------------------------------===//

#if defined(__clang__) || defined(__GNUC__)
extern "C" {
int __cdecl __cxa_atexit(void (*)(void*), void*, void*) CRT_WEAK;
void __cdecl __cxa_finalize(void*) CRT_WEAK;
}

#define CRT_HAS_CXA_ATEXIT() (__cxa_atexit != nullptr)
#define CRT_CXA_ATEXIT_CALL(func, arg, dso) __cxa_atexit(func, arg, dso)
#define CRT_CXA_FINALIZE_CALL(dso)                                             \
  do {                                                                         \
    if (__cxa_finalize)                                                        \
      __cxa_finalize(dso);                                                     \
  } while (0)

#else
// MSVC: use alternatename linker directives
// The noop functions are marked noinline and use volatile to prevent
// the optimizer from removing them before the linker sees them.
extern "C" {
int __cdecl __cxa_atexit(void (*)(void*), void*, void*);
void __cdecl __cxa_finalize(void*);

CRT_NOINLINE int __cdecl crt_cxa_atexit_noop(void (*f)(void*), void* a, void* d) {
  (void)f; (void)a; (void)d;
  return -1;  // Indicate failure (no libc++abi)
}

CRT_NOINLINE void __cdecl crt_cxa_finalize_noop(void* d) {
  (void)d;
  // No-op when libc++abi is not linked
}
}

#pragma comment(linker, "/alternatename:" CRT_SYM_PREFIX "__cxa_atexit="       \
                                          CRT_SYM_PREFIX "crt_cxa_atexit_noop")
#pragma comment(linker, "/alternatename:" CRT_SYM_PREFIX "__cxa_finalize="     \
                                          CRT_SYM_PREFIX "crt_cxa_finalize_noop")

#define CRT_HAS_CXA_ATEXIT() (1)
#define CRT_CXA_ATEXIT_CALL(func, arg, dso) __cxa_atexit(func, arg, dso)
#define CRT_CXA_FINALIZE_CALL(dso) __cxa_finalize(dso)
#endif

//===----------------------------------------------------------------------===//
// DSO handle (defined in crt_windows_init.cpp)
//
// __dso_handle is used by __cxa_atexit to identify which DSO (executable or
// DLL) registered an exit handler. This allows __cxa_finalize to selectively
// run handlers for a specific DSO when it's unloaded.
//
// The value is the address of the symbol itself, which is unique per DSO.
//===----------------------------------------------------------------------===//

extern "C" void* __dso_handle;

//===----------------------------------------------------------------------===//
// CRT section declarations (extern, defined in crt_windows_init.cpp)
//
// These arrays mark the boundaries of function pointer tables placed in
// special PE sections. The linker merges sections alphabetically by suffix:
//
//   __xi_a/__xi_z : .CRT$XIA to .CRT$XIZ - C initializers
//   __xc_a/__xc_z : .CRT$XCA to .CRT$XCZ - C++ constructors
//   __xp_a/__xp_z : .CRT$XPA to .CRT$XPZ - Pre-terminators
//   __xt_a/__xt_z : .CRT$XTA to .CRT$XTZ - Terminators
//
// User code can register callbacks by placing function pointers in these
// sections using #pragma section and __declspec(allocate).
//===----------------------------------------------------------------------===//

extern "C" {
// C initializers (return int, non-zero = error)
extern _PIFV __xi_a[];
extern _PIFV __xi_z[];

// C++ constructors (return void)
extern _PVFV __xc_a[];
extern _PVFV __xc_z[];

// Pre-terminators (run before atexit handlers)
extern _PVFV __xp_a[];
extern _PVFV __xp_z[];

// Terminators (run after atexit handlers)
extern _PVFV __xt_a[];
extern _PVFV __xt_z[];
}

//===----------------------------------------------------------------------===//
// Internal function declarations
//===----------------------------------------------------------------------===//

namespace crt {

// Initialization (crt_windows_init.cpp)
void commonInit();
void initterm(_PVFV* first, _PVFV* last);
int initterm_e(_PIFV* first, _PIFV* last);

// Termination (crt_windows_init.cpp)
void runPreterminators();
void runTerminators();

// Command line (crt_windows_cmdline.cpp)
void initArgsA();
void initArgsW();
void initEnvironA();
void initEnvironW();
LPSTR getWinMainCmdLineA();
LPWSTR getWinMainCmdLineW();
int getArgc();
char** getArgv();
wchar_t** getWargv();
char** getEnviron();
wchar_t** getWenviron();

// Security (crt_windows_security.cpp)
void securityInitCookie();

// Pseudo-relocation (crt_windows_pseudo_reloc.cpp)
// Called by commonInit() before any other initialization
void runPseudoRelocator();

} // namespace crt

//===----------------------------------------------------------------------===//
// Pseudo-relocation support
//
// When using auto-import (-Xlinker -runtime-pseudo-reloc), the linker generates
// pseudo-relocations for data symbols imported from DLLs. These must be
// processed at runtime before the program can use those symbols.
//
// The _pei386_runtime_relocator function is the standard entry point that
// the linker expects to exist when pseudo-relocations are present.
//===----------------------------------------------------------------------===//

extern "C" void _pei386_runtime_relocator(void);

//===----------------------------------------------------------------------===//
// Standard CRT globals (UCRT-owned)
//
// These symbols are defined and owned by UCRT. We import them via dllimport
// and populate them by calling __getmainargs/__wgetmainargs during our init.
// This avoids ODR conflicts and ensures all code sees the same values.
//===----------------------------------------------------------------------===//

extern "C" {
__declspec(dllimport) extern int __argc;
__declspec(dllimport) extern char** __argv;
__declspec(dllimport) extern wchar_t** __wargv;
__declspec(dllimport) extern char** _environ;
__declspec(dllimport) extern wchar_t** _wenviron;
}

//===----------------------------------------------------------------------===//
// Public CRT function exports
//
// These are the standard CRT symbols that external code may reference.
// Defined in crt_windows_init.cpp.
//===----------------------------------------------------------------------===//

extern "C" {

/// Iterate through an array of void(void) function pointers, calling each
/// non-null entry. Used to run C++ constructors and terminators.
/// \param first Pointer to the first element of the array.
/// \param last Pointer past the last element of the array.
void __cdecl _initterm(_PVFV* first, _PVFV* last);

/// Iterate through an array of int(void) function pointers, calling each
/// non-null entry until one returns non-zero (error). Used to run C
/// initializers that can report failures.
/// \param first Pointer to the first element of the array.
/// \param last Pointer past the last element of the array.
/// \returns 0 on success, or the first non-zero return value on failure.
[[nodiscard]] int __cdecl _initterm_e(_PIFV* first, _PIFV* last);

// DLL CRT initialization entry point for custom loaders/mixed-mode assemblies
BOOL __stdcall _CRT_INIT(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved);

// Termination functions
// _cexit: Run pre-terminators and return (doesn't terminate process)
// _c_exit: Return without running any cleanup (for special scenarios)
void __cdecl _cexit(void);
void __cdecl _c_exit(void);

}

#endif // COMPILER_RT_LIB_CRT_CRT_WINDOWS_INTERNAL_H
