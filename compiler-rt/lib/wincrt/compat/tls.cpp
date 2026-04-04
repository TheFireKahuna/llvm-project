//===-- tls.cpp - Thread Local Storage support ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PE/COFF TLS: IMAGE_TLS_DIRECTORY and dynamic TLS initialization.
//
// MUTUAL EXCLUSIVITY: Defines symbols that conflict with vcruntime:
//   _tls_start, _tls_end  - TLS section boundaries
//   _tls_index            - TLS slot index
//   _tls_used             - IMAGE_TLS_DIRECTORY
//   __xl_a, __xl_z        - TLS callback array bounds
//   __xl_dyn_tls_init     - Dynamic TLS initializer callback
//   __xd_a, __xd_z        - Dynamic TLS initializer array bounds
//
// See init.cpp for mutual exclusivity enforcement mechanism.
//
// TLS callbacks (.CRT$XL*) run inside the loader lock. Destructors must avoid
// LoadLibrary/FreeLibrary and lock acquisition that could cause deadlock.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "../internal.h"

// IMAGE_TLS_DIRECTORY: differs by architecture (32-bit vs 64-bit pointers).
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
struct WINCRT_IMAGE_TLS_DIRECTORY {
  unsigned __int64 StartAddressOfRawData;
  unsigned __int64 EndAddressOfRawData;
  unsigned __int64 AddressOfIndex;
  unsigned __int64 AddressOfCallBacks;
  DWORD SizeOfZeroFill;
  DWORD Characteristics;
};
#else
struct WINCRT_IMAGE_TLS_DIRECTORY {
  DWORD StartAddressOfRawData;
  DWORD EndAddressOfRawData;
  DWORD AddressOfIndex;
  DWORD AddressOfCallBacks;
  DWORD SizeOfZeroFill;
  DWORD Characteristics;
};
#endif

// Linker merges .tls$* sections alphabetically; these mark the bounds.
#pragma section(".tls", long, read, write)
#pragma section(".tls$ZZZ", long, read, write)

extern "C" {

__declspec(allocate(".tls")) __declspec(selectany) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) __declspec(selectany) char _tls_end = 0;
// Loader writes the allocated TLS slot index here.
__declspec(selectany) DWORD _tls_index = 0;

}

// TLS callbacks (.CRT$XL*) called by loader on thread/process attach/detach.
// Dynamic TLS initializers (.CRT$XD*) for C++ thread_local variables.
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLB", long, read)
#pragma section(".CRT$XLZ", long, read)
#pragma section(".CRT$XDA", long, read)
#pragma section(".CRT$XDZ", long, read)

extern "C" {

__declspec(allocate(".CRT$XDA")) __declspec(selectany)
    _PVFV __xd_a[] = {nullptr};
__declspec(allocate(".CRT$XDZ")) __declspec(selectany)
    _PVFV __xd_z[] = {nullptr};

}

static void __stdcall __dyn_tls_init(void* hinstDLL, DWORD fdwReason,
                                      void* lpvReserved) {
  (void)hinstDLL;
  (void)lpvReserved;

  if (fdwReason == DLL_THREAD_ATTACH ||
      fdwReason == DLL_PROCESS_ATTACH) {
    for (_PVFV* fn = __xd_a; fn < __xd_z; ++fn) {
      if (*fn)
        (*fn)();
    }
  }
}

extern "C" {

__declspec(allocate(".CRT$XLA")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_a = nullptr;

__declspec(allocate(".CRT$XLB")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_dyn_tls_init =
        reinterpret_cast<PIMAGE_TLS_CALLBACK>(__dyn_tls_init);

__declspec(allocate(".CRT$XLZ")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_z = nullptr;

}

#if defined(__i386__)
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:___xl_dyn_tls_init")
#else
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:__xl_dyn_tls_init")
#endif

// Force linker to include TLS directory.
#pragma section(".rdata$T", long, read)

extern "C" {

__declspec(allocate(".rdata$T")) __declspec(selectany)
    WINCRT_IMAGE_TLS_DIRECTORY _tls_used = {
        reinterpret_cast<uintptr_t>(&_tls_start),
        reinterpret_cast<uintptr_t>(&_tls_end),
        reinterpret_cast<uintptr_t>(&_tls_index),
        reinterpret_cast<uintptr_t>(&__xl_a),
        0,
        0
};

}

#endif // LLVM_RUNTIME_WIN32
