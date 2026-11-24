//===-- entry_dll.cpp - The entry point of a DLL --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

WINCRT_WRAP_UCRT

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID);

// The DllMain of a DLL that defines none.
extern "C" BOOL WINAPI __wincrt_DllMain(HINSTANCE, DWORD, LPVOID) {
  return TRUE;
}
WINCRT_ALTERNATENAME(DllMain, __wincrt_DllMain)

// Whether process attach has initialized the image and not yet been undone.
// The loader serializes the notifications that read and write it.
static bool Attached;

// For a DLL whose own entry point initializes the runtime, as with vcruntime.
extern "C" BOOL WINAPI _CRT_INIT(HINSTANCE, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_ATTACH)
    return Attached = wincrt::initializeImage();
  if (Reason == DLL_PROCESS_DETACH) {
    if (!Attached)
      return FALSE;
    Attached = false;
    // Reserved is null for FreeLibrary and not null while the process
    // terminates.
    wincrt::finalizeImage(Reserved != nullptr);
  }
  return TRUE;
}

// Not exported: each DLL links its own from the archive. The loader takes
// every exception that leaves it, so an Itanium one's raise resumes here.
extern "C" BOOL WINAPI _DllMainCRTStartup(HINSTANCE Instance, DWORD Reason,
                                          LPVOID Reserved) {
  __try {
    if (Reason == DLL_PROCESS_ATTACH) {
      if (!_CRT_INIT(Instance, Reason, Reserved))
        return FALSE;
    } else if (Reason == DLL_PROCESS_DETACH && !Attached) {
      // The loader sends a detach after a failed attach, which was undone
      // then.
      return FALSE;
    }
    BOOL Result = DllMain(Instance, Reason, Reserved);
    if (Reason == DLL_PROCESS_ATTACH && !Result) {
      DllMain(Instance, DLL_PROCESS_DETACH, Reserved);
      Reason = DLL_PROCESS_DETACH;
    }
    // After the user's DllMain, which may still use the image's static
    // objects.
    if (Reason == DLL_PROCESS_DETACH)
      _CRT_INIT(Instance, Reason, Reserved);
    return Result;
  } __except (wincrt::terminateFilter(GetExceptionInformation())) {
    // The filter never selects this handler.
    __builtin_unreachable();
  }
}
