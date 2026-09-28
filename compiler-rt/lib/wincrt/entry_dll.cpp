//===-- entry_dll.cpp - Entry point of a DLL ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID);

// The DllMain of a DLL that defines none.
extern "C" BOOL WINAPI __wincrt_DllMain(HINSTANCE, DWORD, LPVOID) {
  return TRUE;
}
WINCRT_ALTERNATENAME(DllMain, __wincrt_DllMain)

namespace {

// Whether process attach has initialized the image and not yet been undone.
// The loader serializes the notifications that read and write it. After a
// failed attach the loader still sends a detach, which must then do nothing.
bool Attached;

BOOL attach(HINSTANCE Instance, LPVOID Reserved) {
  if (!wincrt::initializeImage())
    return FALSE;
  Attached = true;
  if (DllMain(Instance, DLL_PROCESS_ATTACH, Reserved))
    return TRUE;
  DllMain(Instance, DLL_PROCESS_DETACH, Reserved);
  Attached = false;
  return FALSE;
}

BOOL detach(HINSTANCE Instance, LPVOID Reserved) {
  if (!Attached)
    return FALSE;
  // The user's DllMain may still use the image's static objects.
  BOOL Result = DllMain(Instance, DLL_PROCESS_DETACH, Reserved);
  Attached = false;
  return Result;
}

} // namespace

// Not exported: each DLL links its own from the archive.
extern "C" BOOL WINAPI _DllMainCRTStartup(HINSTANCE Instance, DWORD Reason,
                                          LPVOID Reserved) {
  if (Reason == DLL_PROCESS_ATTACH)
    __security_init_cookie();
  __try {
    switch (Reason) {
    case DLL_PROCESS_ATTACH:
      return attach(Instance, Reserved);
    case DLL_PROCESS_DETACH:
      return detach(Instance, Reserved);
    default:
      return DllMain(Instance, Reason, Reserved);
    }
  } __except (wincrt::terminateFilter(GetExceptionInformation())) {
    // The filter never selects this handler.
    __builtin_unreachable();
  }
}

// For a DLL whose own entry point initializes the runtime, as with vcruntime.
extern "C" BOOL WINAPI _CRT_INIT(HINSTANCE, DWORD Reason, LPVOID) {
  switch (Reason) {
  case DLL_PROCESS_ATTACH:
    Attached = wincrt::initializeImage();
    return Attached;
  case DLL_PROCESS_DETACH:
    if (!Attached)
      return FALSE;
    Attached = false;
    return TRUE;
  default:
    return TRUE;
  }
}
