//===-- tlregdtor.cpp - __tlregdtor and __dyn_tls_dtor --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An MSVC object registers a thread-local's destructor with __tlregdtor, which
// joins the thread's one list, so that its destructors and clang's run in one
// order, newest first, as the thread ends. A file of its own, since an image
// that registers a thread-local destructor keeps the executable's thread-end
// slot.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" {

int __cdecl __tlregdtor(void(__cdecl *Function)(void)) {
  if (!Function)
    return -1;
  return __llvm_kcfi_cxa_thread_atexit_impl(
      reinterpret_cast<wincrt::Destructor>(wincrt::callVoid),
      reinterpret_cast<void *>(Function), &__dso_handle);
}

// Runs the destructors the calling thread registered from this image, as
// vcruntime's per-image list has it; the thread's end runs the rest. No TLS
// callback names it, which would run this image's destructors at its detach,
// ahead of newer ones of images that detach later.
void NTAPI __dyn_tls_dtor(void *, DWORD Reason, void *) {
  if (Reason == DLL_THREAD_DETACH || Reason == DLL_PROCESS_DETACH)
    __cxa_thread_finalize(&__dso_handle);
}

extern const PIMAGE_TLS_CALLBACK __dyn_tls_dtor_callback = __dyn_tls_dtor;

} // extern "C"
