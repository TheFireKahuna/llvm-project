//===-- tls_tail.cpp - The executable's last TLS callback -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The executable's last TLS callback, which the loader runs after every DLL's
// detach and the executable's own callbacks: the executable's export
// __wincrt_thread_tail, which calls the thread-local runtime's
// __wincrt_thread_end, and which the runtime finds there at its attach, so
// that it releases the thread's block here rather than in its own callback.
// The callback array is read-only, so it holds the executable's own function
// rather than an import slot the loader would write. The linker keeps this
// object only in an executable whose callbacks or thread-local destructors
// read thread-locals at a thread's end.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" void NTAPI __wincrt_thread_end(void *, DWORD, void *);

extern "C" __declspec(dllexport) WINCRT_NO_TP void NTAPI
__wincrt_thread_tail(void *Module, DWORD Reason, void *Reserved) {
  __wincrt_thread_end(Module, Reason, Reserved);
}

#pragma section(".CRT$XLY", read)
extern "C" __declspec(allocate(".CRT$XLY"))
const PIMAGE_TLS_CALLBACK __wincrt_thread_end_slot = __wincrt_thread_tail;

// The callbacks are the loader's only through the TLS directory, which code of
// the owned model names nowhere.
#pragma comment(linker, "/include:_tls_used")
