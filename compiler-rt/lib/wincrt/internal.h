//===-- internal.h - Windows CRT internal definitions ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared types, imports, and macros for Windows Itanium CRT runtime.
// Mutually exclusive with vcruntime - replaces MSVC CRT startup entirely.
// Depends on: ucrt.lib, kernel32.lib, bcrypt.lib, ntdll.lib
//
// This CRT bridges UCRT and Win32 with the Itanium C++ ABI, providing:
//   - Entry points (_DllMainCRTStartup, mainCRTStartup, etc.)
//   - CRT section processing (.CRT$X*)
//   - __cxa_atexit / __cxa_finalize implementation
//   - Security cookie support
//   - PE load configuration
//   - Pseudo-relocation for COFF vtables
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_INTERNAL_H
#define COMPILER_RT_LIB_WINCRT_INTERNAL_H

#ifndef LLVM_RUNTIME_WIN32
#error "This file is Windows-only"
#endif

//===----------------------------------------------------------------------===//
// Mutual exclusivity enforcement with vcruntime
//===----------------------------------------------------------------------===//
//
// Linking both wincrt and vcruntime causes undefined behavior due to
// conflicting symbols. Use detect_mismatch to cause a linker error if both
// are present.
//
#if defined(_MSC_VER) || defined(__clang__)
#pragma detect_mismatch("wincrt_crt", "wincrt")
#endif

// Additional enforcement: define a marker symbol.
extern "C" __declspec(selectany) volatile long _wincrt_marker = 0x57494E43; // "WINC"

// Windows headers with portability macros.
// Must set these BEFORE including windows.h.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10+
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>
#include <corecrt_startup.h>
#include <stddef.h>
#include <stdint.h>

// Architecture validation (Clang/GCC macros only).
#if defined(__arm64ec__)
#pragma message("ARM64EC support is experimental")
#elif !(defined(__x86_64__) || defined(__i386__) || \
        defined(__aarch64__) || defined(__arm__))
#error "Unsupported architecture"
#endif

// x86 symbols have underscore prefix.
#if defined(__i386__)
#define WINCRT_SYM_PREFIX "_"
#define WINCRT_DECORATED_NAME(name) _##name
#else
#define WINCRT_SYM_PREFIX ""
#define WINCRT_DECORATED_NAME(name) name
#endif

// Linker alternatename macro.
#define WINCRT_STRINGIFY_(x) #x
#define WINCRT_STRINGIFY(x) WINCRT_STRINGIFY_(x)

#define WINCRT_ALTERNATENAME(weak, strong)                                     \
  __pragma(comment(linker, "/alternatename:"                                   \
                   WINCRT_STRINGIFY(WINCRT_DECORATED_NAME(weak)) "="            \
                   WINCRT_STRINGIFY(WINCRT_DECORATED_NAME(strong))))

// Compiler attributes.
#if defined(__clang__) || defined(__GNUC__)
#define WINCRT_HIDDEN __attribute__((visibility("hidden")))
#define WINCRT_NOINLINE __attribute__((noinline))
#define WINCRT_NORETURN __attribute__((noreturn))
#define WINCRT_USED __attribute__((used))
#define WINCRT_ALIGNED(n) __attribute__((aligned(n)))
#else
#define WINCRT_HIDDEN
#define WINCRT_NOINLINE __declspec(noinline)
#define WINCRT_NORETURN __declspec(noreturn)
#define WINCRT_USED
#define WINCRT_ALIGNED(n) __declspec(align(n))
#endif

// Compiler barrier (prevents reordering across this point).
#if defined(_MSC_VER) || defined(__clang__)
extern "C" void _ReadWriteBarrier(void);
#pragma intrinsic(_ReadWriteBarrier)
#else
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#endif

// ARM64/ARM data memory barrier for cross-thread synchronization.
#if defined(__aarch64__) || defined(__arm64ec__) || defined(__arm__)
// _ARM64_BARRIER_ISH = 0xB (Inner Shareable, full barrier).
#define _ARM64_BARRIER_ISH 0xB
#if defined(_MSC_VER) || defined(__clang__)
extern "C" void __dmb(unsigned int);
#pragma intrinsic(__dmb)
#else
#define __dmb(type) __asm__ __volatile__("dmb ish" ::: "memory")
#endif
#endif

// Diagnostics: WINCRT_TRACE for debug, WINCRT_FATAL always emits for post-mortem.
#if !defined(WINCRT_DEBUG)
#if defined(_DEBUG)
#define WINCRT_DEBUG 1
#else
#define WINCRT_DEBUG 0
#endif
#endif

//===----------------------------------------------------------------------===//
// Thread-local storage limitations
//===----------------------------------------------------------------------===//
//
// CRITICAL: TLS destructor storage is per-module. Static linking libc++ into
// multiple DLLs causes thread_local destructors to be missed on thread exit.
// See cxa_thread_atexit.cpp for details.
//

#if WINCRT_DEBUG

#define WINCRT_ASSERT(cond)                                                    \
  do {                                                                         \
    if (!(cond)) {                                                             \
      OutputDebugStringA("WINCRT ASSERTION FAILED: " #cond "\n");              \
      __debugbreak();                                                          \
    }                                                                          \
  } while (0)

#define WINCRT_TRACE(msg)                                                      \
  do {                                                                         \
    OutputDebugStringA("WINCRT: " msg "\n");                                   \
  } while (0)

#else

#define WINCRT_ASSERT(cond) ((void)0)
#define WINCRT_TRACE(msg) ((void)0)

#endif

// Always emits for post-mortem debugging.
#define WINCRT_FATAL(msg)                                                      \
  do {                                                                         \
    OutputDebugStringA("WINCRT FATAL: " msg "\n");                             \
  } while (0)

// Exit and error codes.
namespace wincrt {

enum class ExitCode : unsigned {
  Success = 0,
  Purecall = 3,
  AllocFailure = 254,
  InitFailure = 255
};

inline unsigned toUnsigned(ExitCode code) {
  return static_cast<unsigned>(code);
}

// UCRT _amsg_exit() codes matching _RT_* from crtdefs.h.
enum class RuntimeError : int {
  SpaceArg = 8,
  SpaceEnv = 9,
  Purecall = 25,
  CrtNotInit = 30,
  PseudoRelocFailed = 100,
  TlsInitFailed = 102,
  InvalidParameter = 104,
};

} // namespace wincrt

// Constants.
namespace wincrt {

// Linker looks for _fltused when floating-point code is present.
constexpr int FltusedMagic = 0x9875;
constexpr size_t CacheLineSize = 64;

// __fastfail() codes from winnt.h (SDK 10.0.26100.0).
enum class FastFail : unsigned {
  StackCookieCheckFailure = 2,
  InvalidArg = 5,
  GsCookieInit = 6,
  FatalAppExit = 7,
  RangeCheckFailure = 8,
  GuardIcallCheckFailure = 10,
  DloadProtectionFailure = 25,
  InvalidFlsData = 70,
};

// Security cookie defaults from MSVC gs_cookie.c.
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
using CookieType = uint64_t;
constexpr CookieType DefaultSecurityCookie = 0x00002B992DDFA232ULL;
#else
using CookieType = uint32_t;
constexpr CookieType DefaultSecurityCookie = 0xBB40E64EUL;
constexpr CookieType CookieHighWordMask = 0xFFFF0000UL;
constexpr CookieType CookieHighWordFixup = 0x4711UL;
#endif

static_assert(sizeof(CookieType) == sizeof(void*), "cookie size mismatch");

} // namespace wincrt


// Control Flow Guard (CFG) constants. SDK provides IMAGE_GUARD_* but these
// are the names used in LLVM BinaryFormat/COFF.h for consistency.
namespace wincrt {
namespace GuardFlags {

constexpr DWORD CF_INSTRUMENTED = 0x00000100;
constexpr DWORD CFW_INSTRUMENTED = 0x00000200;
constexpr DWORD CF_FUNCTION_TABLE_PRESENT = 0x00000400;
constexpr DWORD SECURITY_COOKIE_UNUSED = 0x00000800;
constexpr DWORD PROTECT_DELAYLOAD_IAT = 0x00001000;
constexpr DWORD DELAYLOAD_IAT_IN_ITS_OWN_SECTION = 0x00002000;
constexpr DWORD CF_EXPORT_SUPPRESSION_INFO_PRESENT = 0x00004000;
constexpr DWORD CF_ENABLE_EXPORT_SUPPRESSION = 0x00008000;
constexpr DWORD CF_LONGJUMP_TABLE_PRESENT = 0x00010000;
constexpr DWORD RF_INSTRUMENTED = 0x00020000;
constexpr DWORD RF_ENABLE = 0x00040000;
constexpr DWORD RF_STRICT = 0x00080000;
constexpr DWORD EH_CONTINUATION_TABLE_PRESENT = 0x00400000;
constexpr DWORD XFG_ENABLED = 0x00800000;
constexpr DWORD CF_FUNCTION_TABLE_SIZE_MASK = 0xF0000000;
constexpr DWORD CF_FUNCTION_TABLE_SIZE_SHIFT = 28;
constexpr DWORD DEFAULT_CFG = CF_INSTRUMENTED | CF_FUNCTION_TABLE_PRESENT;

} // namespace GuardFlags
} // namespace wincrt

// Function pointer types matching UCRT conventions.
typedef int (__cdecl *_PIFV)(void);   // C initializers
typedef void (__cdecl *_PVFV)(void);  // C++ constructors/destructors
typedef void (__cdecl *_purecall_handler)(void);
typedef void (__cdecl *_invalid_parameter_handler)(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t);
typedef void (__stdcall *PIMAGE_TLS_CALLBACK)(void*, DWORD, void*);


// Minimum OS version requirement.
namespace wincrt {

// RS5 required for _register_thread_local_exe_atexit_callback (UCRT), which
// enables running __cxa_finalize before C atexit handlers on process exit.
// Instead of checking version via ntdll!RtlGetVersion, we probe for the API
// directly - avoids ntdll dependency and manifest issues with GetVersionEx.
inline void verifyMinimumWindowsVersion() {
  HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
  if (ucrt && GetProcAddress(ucrt, "_register_thread_local_exe_atexit_callback"))
    return;

  OutputDebugStringA("FATAL: Windows 10 RS5 (build 17763) or later required\n");
  __fastfail(static_cast<unsigned>(FastFail::InvalidArg));
}

} // namespace wincrt

// UCRT imports. Wincrt delegates stdio, malloc, errno to ucrtbase.dll.
// Startup types and functions are declared in <corecrt_startup.h>.
extern "C" {
__declspec(dllimport) WINCRT_NORETURN void __cdecl abort(void);
__declspec(dllimport) WINCRT_NORETURN void __cdecl _exit(int);
// _amsg_exit is NOT exported from ucrtbase.dll; wincrt provides it in init.cpp.
WINCRT_NORETURN void __cdecl _amsg_exit(int);
__declspec(dllimport) void __cdecl _fpreset(void);
}

// SDK: process.h:53-54. Callback runs before atexit for Itanium LIFO ordering.
using _tls_callback_type = void(__stdcall*)(void*, DWORD, void*);

extern "C" {
__declspec(dllimport) void __cdecl
    _register_thread_local_exe_atexit_callback(_tls_callback_type);
}


namespace wincrt {

// Customer exception code: 0xC0004352 ("CR" + customer bit).
constexpr DWORD kFatalExceptionCode = 0xC0004352;

} // namespace wincrt

// Error handling: securityFailure for uninterceptable __fastfail,
// fatalError for UCRT _amsg_exit, fatalErrorEarly before UCRT is available.
namespace wincrt {

/// Terminate via __fastfail. No cleanup, no interception.
WINCRT_NORETURN inline void securityFailure(FastFail code) {
  __fastfail(static_cast<unsigned>(code));
}

/// Terminate with diagnostic message.
WINCRT_NORETURN inline void securityFailure(const char* msg, FastFail code) {
  OutputDebugStringA("WINCRT FATAL: ");
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  __fastfail(static_cast<unsigned>(code));
}

/// Terminate via UCRT _amsg_exit. Requires UCRT initialized.
WINCRT_NORETURN inline void fatalError(RuntimeError errCode) {
  _amsg_exit(static_cast<int>(errCode));
}

/// Terminate with message via UCRT.
WINCRT_NORETURN inline void fatalError(const char* msg, RuntimeError errCode) {
  OutputDebugStringA("WINCRT FATAL: ");
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  _amsg_exit(static_cast<int>(errCode));
}

inline void writeStderr(const char* msg) {
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE)
    return;

  DWORD written;
  DWORD len = 0;
  for (const char* p = msg; *p; ++p, ++len);
  WriteFile(h, msg, len, &written, nullptr);
}

/// Terminate before UCRT available. Uses RaiseFailFastException for WER.
WINCRT_NORETURN inline void fatalErrorEarly(const char* msg) {
  OutputDebugStringA("WINCRT FATAL: ");
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  writeStderr("WINCRT FATAL: ");
  writeStderr(msg);
  writeStderr("\n");

  EXCEPTION_RECORD er = {};
  er.ExceptionCode = kFatalExceptionCode;
  er.NumberParameters = 1;
  er.ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(msg);
  RaiseFailFastException(&er, nullptr, 0);
  __builtin_unreachable();
}

/// Invoke a destructor, calling terminate() if it throws.
/// Per Itanium ABI 3.3.5: throwing from __cxa_finalize calls terminate().
///
/// Defined in dtor_call.cpp, which is compiled with -fexceptions to handle
/// both SEH and SJLJ exception models correctly.
void invokeDestructorImpl(void (*dtor)(void*), void* obj, const char* context);

inline void invokeDestructor(void (*dtor)(void*), void* obj,
                             const char* context) {
  invokeDestructorImpl(dtor, obj, context);
}

} // namespace wincrt

// Itanium ABI exit handlers. Resolved from LLVM libc if linked, otherwise from
// wincrt fallback via /alternatename linker directive.
extern "C" {
int __cdecl __cxa_atexit(void (*)(void*), void*, void*);
void __cdecl __cxa_finalize(void*);
int __cxa_thread_atexit_impl(void (*)(void*), void*, void*);
void __cxa_thread_finalize(void*);
void __cxa_thread_finalize_dso_unload(void*);
}

extern "C" void* __dso_handle;

// CRT section declarations. Linker merges .CRT$X?A to .CRT$X?Z alphabetically.
extern "C" {
extern _PIFV __xi_a[];  // .CRT$XIA - C initializers
extern _PIFV __xi_z[];  // .CRT$XIZ
extern _PVFV __xc_a[];  // .CRT$XCA - C++ constructors
extern _PVFV __xc_z[];  // .CRT$XCZ
extern _PVFV __xp_a[];  // .CRT$XPA - Pre-terminators
extern _PVFV __xp_z[];  // .CRT$XPZ
extern _PVFV __xt_a[];  // .CRT$XTA - Terminators
extern _PVFV __xt_z[];  // .CRT$XTZ
}

// Destructor block storage for __cxa_atexit. Design from LLVM libc's
// ReverseOrderBlockStore: first block inline, subsequent blocks heap-allocated.
namespace wincrt {

/// Entry for destructor registration. Compatible with Itanium ABI 3.3.5.
struct DtorEntry {
  void (*Dtor)(void*);
  void* Obj;
  void* Dso;
};

constexpr size_t kDtorBlockSize = 32;

struct DtorBlock {
  DtorEntry Entries[kDtorBlockSize];
  DtorBlock* Next;
  size_t Count;

  bool hasSpace() const { return Count < kDtorBlockSize; }

  void push(void (*dtor)(void*), void* obj, void* dso) {
    WINCRT_ASSERT(Count < kDtorBlockSize);
    Entries[Count].Dtor = dtor;
    Entries[Count].Obj = obj;
    Entries[Count].Dso = dso;
    ++Count;
  }
};

// Heap helpers using process heap (no custom allocator).
inline void* crtAlloc(size_t size) {
  return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

inline void crtFree(void* ptr) {
  if (ptr)
    HeapFree(GetProcessHeap(), 0, ptr);
}

inline DtorBlock* allocateDtorBlock() {
  return static_cast<DtorBlock*>(crtAlloc(sizeof(DtorBlock)));
}

inline void freeDtorBlock(DtorBlock* block) {
  crtFree(block);
}

} // namespace wincrt

// Internal function declarations.
namespace wincrt {

void commonInit();
void runPreterminators();
void runTerminators();

// Exit synchronization. Used by _cexit() and the UCRT exit callback.
// tryBeginExitCleanup returns true if this thread should run cleanup.
// runExitCleanup performs Itanium ABI cleanup in the correct order.
bool tryBeginExitCleanup();
void runExitCleanup();

void securityInitCookie();

} // namespace wincrt

// Control Flow Guard symbols. Loader patches with ntdll validation.
// Must be volatile - loader modifies at runtime, compiler must not cache.
extern "C" {

extern volatile void* __guard_check_icall_fptr;
extern volatile void* __guard_dispatch_icall_fptr;
extern DWORD __guard_flags;

#if defined(__arm64ec__)
extern void* __os_arm64x_check_icall;
extern void* __os_arm64x_check_icall_cfg;
extern void* __os_arm64x_dispatch_call;
extern void* __os_arm64x_dispatch_call_no_redirect;
#endif

}

extern "C" void _pei386_runtime_relocator(void);

namespace wincrt {
// Provided by builtins (crt_pseudo_reloc_windows.cpp).
inline void runPseudoRelocator() { _pei386_runtime_relocator(); }
} // namespace wincrt

// UCRT argument/environment accessors. UCRT exports __p___argc() style functions
// rather than direct data exports. These return pointers to the actual globals.
extern "C" {
__declspec(dllimport) int* __cdecl __p___argc(void);
__declspec(dllimport) char*** __cdecl __p___argv(void);
__declspec(dllimport) wchar_t*** __cdecl __p___wargv(void);
__declspec(dllimport) char*** __cdecl __p__environ(void);
__declspec(dllimport) wchar_t*** __cdecl __p__wenviron(void);
}

// Convenience accessors matching traditional CRT globals.
namespace wincrt {
inline int& argc() { return *__p___argc(); }
inline char**& argv() { return *__p___argv(); }
inline wchar_t**& wargv() { return *__p___wargv(); }
inline char**& environ() { return *__p__environ(); }
inline wchar_t**& wenviron() { return *__p__wenviron(); }
} // namespace wincrt

// Public functions.
extern "C" {
BOOL __stdcall _CRT_INIT(HINSTANCE, DWORD, LPVOID);
void __cdecl _cexit(void);
void __cdecl _c_exit(void);
int __cdecl _is_c_termination_complete(void);
}

#endif // COMPILER_RT_LIB_WINCRT_INTERNAL_H
