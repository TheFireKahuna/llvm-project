//===-- Shared DllMain skeleton for Windows ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Mechanical boilerplate shared between c.dll's `libc_dll_entry.cpp` and
// the generic user-DLL skeleton in `dll_startup.cpp`. Only the pieces
// that are truly identical between the two live here; the bring-up and
// teardown policy (whether to call __libc_bootstrap / __libc_dll_init,
// whether to walk .CRT$XI/XP/XT sections) stays in each entry-point TU
// because that's the actual difference between c.dll and a user DLL.
//
// Header-only and freestanding-safe — no libc headers, no Win32 headers.
// All Windows types are expressed as plain C types so this can be
// included before any Tier B init is available.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_STARTUP_WINDOWS_DLL_MAIN_COMMON_H
#define LLVM_LIBC_STARTUP_WINDOWS_DLL_MAIN_COMMON_H

#include "include/__llvm-libc-common.h"

// Per-module DSO handle — set to HINSTANCE on DLL_PROCESS_ATTACH so
// __cxa_finalize can run only this DLL's dtors. Each entry-point TU
// includes this header and gets its own selectany definition; the
// linker keeps exactly one per output DLL.
extern "C" void *__dso_handle;
__LIBC_SELECTANY_ATTR void *__dso_handle = nullptr;

// Itanium ABI cleanup. __cxa_finalize is provided by atexit.cpp,
// __cxa_thread_finalize_dso_unload by the thread finalize implementation;
// both pulled into the link via WHOLEARCHIVE on the libc side, or via
// dll_crt.cpp's bundled fallbacks for non-libc DLLs.
extern "C" void __cxa_finalize(void *dso);
extern "C" void __cxa_thread_finalize_dso_unload(void *dso);

// Security cookie init — must run before any /GS-protected function. On
// c.dll this is also covered by the .CRT$XLAA TLS callback as
// defense-in-depth; on user DLLs it's the only call site.
extern "C" void __security_init_cookie(void);

// Default DllMain — returns TRUE. The entrypoint TU that opts into the
// fallback emits the concrete symbol body via LIBC_DLL_INSTALL_DEFAULT_DLLMAIN;
// this keeps the startup owner self-contained and guarantees a real external
// symbol for the linker /alternatename hook. LIBC_MSABI pins the calling
// convention because DllMain is invoked by the PE loader with MS x64 ABI,
// irrespective of the TU default.
extern "C" LIBC_MSABI int /* BOOL */ __libc_DefaultDllMain(
    void * /* HINSTANCE */, unsigned long /* DWORD */, void * /* LPVOID */);

// COFF weak external — if the hosting DLL defines DllMain, the user's
// strong definition wins; otherwise the linker resolves DllMain to
// __libc_DefaultDllMain via the aux record (IMAGE_SYM_CLASS_WEAK_EXTERNAL).
// Wrapped in a macro so each entry-point TU emits the default body and
// its weak alias exactly once. Trailing `static_assert(true, "")` gives
// the macro a consumable semicolon at namespace scope.

#define LIBC_DLL_INSTALL_DEFAULT_DLLMAIN()                                     \
  extern "C" LIBC_MSABI int /* BOOL */ __libc_DefaultDllMain(                  \
      void * /* HINSTANCE */, unsigned long /* DWORD */,                       \
      void * /* LPVOID */) {                                                   \
    return 1 /* TRUE */;                                                       \
  }                                                                            \
  extern "C" __attribute__((weak, alias("__libc_DefaultDllMain")))             \
  LIBC_MSABI int                                                               \
  /* BOOL */ DllMain(void * /* HINSTANCE */, unsigned long /* DWORD */,        \
                     void * /* LPVOID */);                                     \
  static_assert(true, "")

// Forward-declared user DllMain — resolved either by the user's own
// definition or by /alternatename → __libc_DefaultDllMain. Each
// entry-point TU still emits the call directly, so signature is shared
// here for consistency.
extern "C" LIBC_MSABI int /* BOOL */ DllMain(void * /* HINSTANCE */,
                                             unsigned long /* DWORD */,
                                             void * /* LPVOID */);

// DLL_PROCESS_ATTACH / DLL_PROCESS_DETACH constants (avoid pulling
// <windows.h>).
inline constexpr unsigned long LIBC_DLL_PROCESS_ATTACH = 1;
inline constexpr unsigned long LIBC_DLL_PROCESS_DETACH = 0;

#endif // LLVM_LIBC_STARTUP_WINDOWS_DLL_MAIN_COMMON_H
