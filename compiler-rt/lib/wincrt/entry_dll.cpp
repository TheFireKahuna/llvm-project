//===-- entry_dll.cpp - DLL entry point -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

// Runtime and library DLLs have no user DllMain; the linker substitutes this
// one when nothing else defines the symbol.
extern "C" BOOL __stdcall __wincrt_DefaultDllMain(HINSTANCE, DWORD, LPVOID) {
  return TRUE;
}
WINCRT_ALTERNATENAME(DllMain, __wincrt_DefaultDllMain)
extern "C" BOOL __stdcall DllMain(HINSTANCE, DWORD, LPVOID);

namespace {

BOOL dispatch(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  __try {
    if (Reason == DLL_PROCESS_ATTACH && !wincrt::initializeImage())
      return FALSE;
    BOOL Result = DllMain(Instance, Reason, Reserved);
    // User code in DllMain may still use this image's static objects; they
    // are destroyed afterwards, as vcruntime does. Reserved is null for
    // FreeLibrary and non-null while the process terminates.
    if (Reason == DLL_PROCESS_DETACH)
      wincrt::detachImage(Reserved != nullptr);
    return Result;
  } __except (wincrt::terminateFilter(GetExceptionInformation())) {
    abort();
  }
}

} // namespace

// Not exported: each DLL must extract its own startup from this archive so
// that /entry never binds to another DLL's import.
extern "C" BOOL __stdcall _DllMainCRTStartup(HINSTANCE Instance, DWORD Reason,
                                             LPVOID Reserved) {
  if (Reason == DLL_PROCESS_ATTACH)
    __security_init_cookie();
  return dispatch(Instance, Reason, Reserved);
}
