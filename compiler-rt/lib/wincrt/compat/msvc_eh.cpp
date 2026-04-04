//===-- msvc_eh.cpp - MSVC/Itanium exception interop ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MSVC and Itanium C++ exception interoperability.
//
// BACKGROUND:
// Windows has two C++ exception ABIs:
//   MSVC:    _CxxThrowException → SEH with code 0xE06D7363
//   Itanium: __cxa_throw → _Unwind_RaiseException → SEH 0x20474343
//
// Both use SEH as the underlying mechanism. With the libunwind modification
// in Unwind-seh.cpp, Itanium catch(...) can now catch MSVC exceptions.
//
// BEHAVIOR:
// When MSVC code throws and Itanium code has catch(...):
//   1. MSVC's _CxxThrowException raises SEH 0xE06D7363
//   2. libunwind's _GCC_specific_handler wraps it as ForeignExceptionWrapper
//   3. libc++abi's personality sees catch(...) and returns _URC_HANDLER_FOUND
//   4. SEH unwinds to the catch block, running destructors
//   5. Itanium catch(...) executes with the foreign exception
//
// LIMITATIONS:
//   - catch(specific_type&) won't catch MSVC exceptions (no type info bridge)
//   - The exception object is opaque; can't access MSVC exception members
//   - Rethrowing with `throw;` re-raises as foreign, not original MSVC exception
//
// DIAGNOSTICS:
// This file provides utilities to detect and diagnose MSVC exceptions caught
// by Itanium handlers.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "../internal.h"

#pragma comment(lib, "ntdll.lib")

extern "C" __declspec(dllimport) PVOID NTAPI
RtlAddVectoredExceptionHandler(ULONG First,
                               PVECTORED_EXCEPTION_HANDLER Handler);

namespace wincrt {
namespace eh {

// MSVC C++ exception code. ASCII "msc" + 0xE0000000.
constexpr DWORD kMsvcCxxExceptionCode = 0xE06D7363;

// Itanium exception codes used by libunwind.
constexpr DWORD kItaniumThrowCode = 0x20474343;  // STATUS_GCC_THROW
constexpr DWORD kItaniumUnwindCode = 0x21474343; // STATUS_GCC_UNWIND

// MSVC exception magic numbers (from ehdata_values.h).
constexpr DWORD kMsvcMagic1 = 0x19930520;
constexpr DWORD kMsvcMagic2 = 0x19930521;
constexpr DWORD kMsvcMagic3 = 0x19930522;

// Determine if an exception code is MSVC C++.
inline bool isMsvcCxxException(DWORD code) {
  return code == kMsvcCxxExceptionCode;
}

// Determine if an exception code is Itanium C++.
inline bool isItaniumException(DWORD code) {
  return code == kItaniumThrowCode || code == kItaniumUnwindCode;
}

} // namespace eh
} // namespace wincrt


//===----------------------------------------------------------------------===//
// Exception tracking via VEH
//===----------------------------------------------------------------------===//
//
// VEH runs before frame handlers, allowing us to record exception info
// before it's caught. This is useful for diagnostics in catch(...) blocks.
//

namespace {

struct MsvcExceptionInfo {
  DWORD code;
  DWORD magic;
  void *object;
  void *throwInfo;
  void *imageBase; // For 64-bit, ThrowInfo is image-relative
};

__declspec(thread) MsvcExceptionInfo g_lastMsvcException = {};
__declspec(thread) bool g_inMsvcException = false;

LONG CALLBACK msvcExceptionTracker(LPEXCEPTION_POINTERS ExInfo) {
  if (!ExInfo || !ExInfo->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;

  DWORD code = ExInfo->ExceptionRecord->ExceptionCode;

  if (wincrt::eh::isMsvcCxxException(code)) {
    g_inMsvcException = true;
    g_lastMsvcException.code = code;

    // MSVC exception layout (from ehdata.h EHExceptionRecord):
    // ExceptionInformation[0] = magic number (0x19930520, 0x19930521, 0x19930522)
    // ExceptionInformation[1] = exception object pointer
    // ExceptionInformation[2] = ThrowInfo pointer
    // ExceptionInformation[3] = image base (64-bit only, for relative offsets)
    if (ExInfo->ExceptionRecord->NumberParameters >= 3) {
      g_lastMsvcException.magic =
          static_cast<DWORD>(ExInfo->ExceptionRecord->ExceptionInformation[0]);
      g_lastMsvcException.object =
          reinterpret_cast<void *>(ExInfo->ExceptionRecord->ExceptionInformation[1]);
      g_lastMsvcException.throwInfo =
          reinterpret_cast<void *>(ExInfo->ExceptionRecord->ExceptionInformation[2]);
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
      if (ExInfo->ExceptionRecord->NumberParameters >= 4) {
        g_lastMsvcException.imageBase =
            reinterpret_cast<void *>(ExInfo->ExceptionRecord->ExceptionInformation[3]);
      }
#endif
    }
  } else {
    g_inMsvcException = false;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

#pragma section(".CRT$XIC", long, read)

int __cdecl installMsvcExceptionTracker(void) {
  // Priority 1 = run first, before other VEH handlers.
  RtlAddVectoredExceptionHandler(1, msvcExceptionTracker);
  return 0;
}

__declspec(allocate(".CRT$XIC")) static _PIFV g_initMsvcTracker =
    installMsvcExceptionTracker;

} // namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

extern "C" {

/// Returns true if current catch(...) caught an MSVC exception.
/// Call from within a catch(...) block to determine exception origin.
int __cdecl _wincrt_is_msvc_exception(void) {
  return g_inMsvcException ? 1 : 0;
}

/// Get the MSVC exception code (0xE06D7363 for C++ exceptions).
DWORD __cdecl _wincrt_get_msvc_exception_code(void) {
  return g_lastMsvcException.code;
}

/// Get the MSVC exception object pointer.
/// WARNING: This is an MSVC-ABI object; its layout depends on the thrown type.
/// For std::exception derivatives, you may be able to call what() via vtable.
void *__cdecl _wincrt_get_msvc_exception_object(void) {
  return g_lastMsvcException.object;
}

/// Get the MSVC ThrowInfo pointer (describes the thrown type).
/// This is an internal MSVC structure; see ehdata.h for layout.
void *__cdecl _wincrt_get_msvc_throw_info(void) {
  return g_lastMsvcException.throwInfo;
}

/// Clear MSVC exception tracking state.
/// Call after handling to reset for next exception.
void __cdecl _wincrt_clear_msvc_exception(void) {
  g_inMsvcException = false;
  g_lastMsvcException = {};
}

/// Attempt to get the exception message if it's a std::exception derivative.
/// Returns nullptr if not applicable or not accessible.
///
/// This works by assuming MSVC std::exception has a vtable with what() at
/// a known offset. This is fragile and may break with different MSVC versions.
const char *__cdecl _wincrt_try_get_msvc_exception_what(void) {
  if (!g_inMsvcException || !g_lastMsvcException.object)
    return nullptr;

  // MSVC std::exception vtable layout (typical):
  //   [0] = destructor
  //   [1] = what()
  // This is ABI-dependent and may not work for all exception types.
  __try {
    void *obj = g_lastMsvcException.object;
    void **vtable = *reinterpret_cast<void ***>(obj);
    if (!vtable)
      return nullptr;

    // what() is typically at vtable[1] for std::exception
    using WhatFn = const char *(__thiscall *)(void *);
    WhatFn whatFn = reinterpret_cast<WhatFn>(vtable[1]);
    if (!whatFn)
      return nullptr;

    return whatFn(obj);
  } __except (1) { // EXCEPTION_EXECUTE_HANDLER
    return nullptr;
  }
}

/// Diagnostic output describing the current MSVC exception.
void __cdecl _wincrt_describe_msvc_exception(void) {
  if (!g_inMsvcException) {
    OutputDebugStringA("WINCRT: No MSVC exception active\n");
    return;
  }

  OutputDebugStringA("WINCRT: MSVC C++ exception caught by Itanium catch(...)\n");

  const char *what = _wincrt_try_get_msvc_exception_what();
  if (what) {
    OutputDebugStringA("WINCRT: Exception message: ");
    OutputDebugStringA(what);
    OutputDebugStringA("\n");
  }
}

} // extern "C"

#endif // LLVM_RUNTIME_WIN32
