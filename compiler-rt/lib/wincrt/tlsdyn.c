//===-- tlsdyn.c - Dynamic initialization of the loader's thread-locals ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What an MSVC object's dynamically initialized thread-local needs: its
// initializer is an entry of .CRT$XD*, which __dyn_tls_init runs once per
// thread under __tls_guard, and its accesses test __tls_guard and call
// __dyn_tls_on_demand_init, which serves a thread that predates the image's
// load. __tls_guard is a thread-local of the loader's template, so this file
// is compiled for the MSVC environment: an object that states no thread-local
// model of its own, as the objects that name these symbols are.
//
//===----------------------------------------------------------------------===//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef void(__cdecl *Initializer)(void);

#pragma section(".CRT$XDA", read)
#pragma section(".CRT$XDZ", read)
#pragma section(".CRT$XCY", read)
#pragma section(".CRT$XLC", read)

__declspec(allocate(".CRT$XDA")) static const Initializer First = 0;
__declspec(allocate(".CRT$XDZ")) static const Initializer Last = 0;

__declspec(thread) unsigned char __tls_guard;
extern ULONG _tls_index;

// The guard is set before any initializer runs, so that one that reaches
// another of the image's thread-locals runs none twice.
void NTAPI __dyn_tls_init(PVOID Module, DWORD Reason, PVOID Reserved) {
  (void)Module;
  (void)Reserved;
  if (Reason != DLL_THREAD_ATTACH || __tls_guard)
    return;
  __tls_guard = 1;
  // The bounds are distinct objects, which the compiler must not compare as
  // such.
  const Initializer *I = &First + 1;
  __asm__("" : "+r"(I));
  for (; I != &Last; ++I)
    if (*I)
      (*I)();
}

const PIMAGE_TLS_CALLBACK __dyn_tls_init_callback = __dyn_tls_init;
__declspec(allocate(".CRT$XLC")) const PIMAGE_TLS_CALLBACK __dyn_tls_init_tls =
    __dyn_tls_init;

void __cdecl __dyn_tls_on_demand_init(void) {
  __dyn_tls_init(NULL, DLL_THREAD_ATTACH, NULL);
}

// The thread that starts the process, or loads the DLL, gets no
// DLL_THREAD_ATTACH: it runs the initializers after the image's constructors.
__declspec(allocate(".CRT$XCY")) const Initializer __dyn_tls_init_main =
    __dyn_tls_on_demand_init;

// What a DLL that exports a thread-local exports, so that an importer's
// access initializes the exporter's thread-locals on its thread first.
unsigned long __cdecl __safe_get_tls_index(void) {
  __dyn_tls_on_demand_init();
  return _tls_index;
}
