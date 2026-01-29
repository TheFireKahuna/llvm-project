//===-- crt_windows_tls.cpp - Thread Local Storage support ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread Local Storage (TLS) support for Windows.
//
// Provides __declspec(thread) / thread_local storage support. The PE/COFF
// loader uses the IMAGE_TLS_DIRECTORY structure to initialize TLS slots for
// each thread.
//
// Components:
// - _tls_start/_tls_end: Bounds of the .tls section (TLS template data)
// - _tls_index: Slot index allocated by the loader (or via TlsAlloc fallback)
// - _tls_used: IMAGE_TLS_DIRECTORY structure embedded in the PE header
// - TLS callbacks: Array of functions called on thread attach/detach
//
// Reference:
// - Microsoft PE/COFF Specification, section "The .tls Section"
// - MSVC CRT tlssup.c
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "crt_windows_internal.h"

//===----------------------------------------------------------------------===//
// IMAGE_TLS_DIRECTORY structure
//===----------------------------------------------------------------------===//

#if defined(CRT_ARCH_X64) || defined(CRT_ARCH_ARM64)
struct CRT_IMAGE_TLS_DIRECTORY {
  unsigned __int64 StartAddressOfRawData;
  unsigned __int64 EndAddressOfRawData;
  unsigned __int64 AddressOfIndex;
  unsigned __int64 AddressOfCallBacks;
  DWORD SizeOfZeroFill;
  DWORD Characteristics;
};
#else
struct CRT_IMAGE_TLS_DIRECTORY {
  DWORD StartAddressOfRawData;
  DWORD EndAddressOfRawData;
  DWORD AddressOfIndex;
  DWORD AddressOfCallBacks;
  DWORD SizeOfZeroFill;
  DWORD Characteristics;
};
#endif

//===----------------------------------------------------------------------===//
// TLS section bounds
//
// The linker merges sections alphabetically by the $ suffix:
//   .tls        (no suffix, comes first)
//   .tls$*      (user's __declspec(thread) variables)
//   .tls$ZZZ    (comes last)
//===----------------------------------------------------------------------===//

#pragma section(".tls", long, read, write)
#pragma section(".tls$ZZZ", long, read, write)

extern "C" {

__declspec(allocate(".tls")) __declspec(selectany) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) __declspec(selectany) char _tls_end = 0;

// TLS index - the loader writes the allocated TLS slot number here
__declspec(selectany) DWORD _tls_index = 0;

}

//===----------------------------------------------------------------------===//
// TLS callbacks array
//
// The linker merges .CRT$XL* sections alphabetically:
//   .CRT$XLA  - start sentinel (nullptr, marks beginning)
//   .CRT$XLB  - early callbacks (our __dyn_tls_init goes here)
//   .CRT$XLC  - user callbacks registered via #pragma
//   .CRT$XLD  - late callbacks
//   .CRT$XLZ  - end sentinel (nullptr, marks end)
//
// The PE loader calls all non-null entries in this array on:
//   DLL_PROCESS_ATTACH - when the module is loaded
//   DLL_THREAD_ATTACH  - when a new thread is created
//   DLL_THREAD_DETACH  - when a thread exits
//   DLL_PROCESS_DETACH - when the module is unloaded
//
// Our __dyn_tls_init callback handles dynamic TLS initialization for
// C++ thread_local variables with non-trivial constructors/destructors.
//===----------------------------------------------------------------------===//

#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLB", long, read)
#pragma section(".CRT$XLZ", long, read)

//===----------------------------------------------------------------------===//
// Dynamic TLS initialization
//
// For C++ thread_local variables with non-trivial constructors, the compiler
// generates initialization code that must run on each thread. This callback
// triggers that initialization.
//
// The compiler places dynamic TLS initializers in the .CRT$XD* sections:
//   .CRT$XDA - start sentinel
//   .CRT$XDZ - end sentinel
//
// We iterate through these and call each initializer for THREAD_ATTACH.
//===----------------------------------------------------------------------===//

#pragma section(".CRT$XDA", long, read)
#pragma section(".CRT$XDZ", long, read)

extern "C" {

// Dynamic TLS initializer array bounds
__declspec(allocate(".CRT$XDA")) __declspec(selectany)
    _PVFV __xd_a[] = {nullptr};
__declspec(allocate(".CRT$XDZ")) __declspec(selectany)
    _PVFV __xd_z[] = {nullptr};

}

// Dynamic TLS initialization callback
// Called by the PE loader for each thread attach/detach event
static void __stdcall __dyn_tls_init(void* hinstDLL, DWORD fdwReason,
                                      void* lpvReserved) {
  (void)hinstDLL;
  (void)lpvReserved;

  if (fdwReason == crt::kDllThreadAttach ||
      fdwReason == crt::kDllProcessAttach) {
    // Initialize dynamic thread_local variables for this thread.
    // Iterate through all registered initializers and call them.
    for (_PVFV* fn = __xd_a; fn < __xd_z; ++fn) {
      if (*fn)
        (*fn)();
    }
  }

  // Note: Thread-local destructors are handled separately via __cxa_thread_atexit
  // (provided by libc++abi) or atexit-style registration. We don't need to
  // explicitly run destructors here because:
  // 1. __cxa_thread_atexit registers per-thread cleanup with the OS
  // 2. The OS ensures thread-local destructors run before thread exit
}

extern "C" {

// Start sentinel
__declspec(allocate(".CRT$XLA")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_a = nullptr;

// Our dynamic TLS initializer - placed early in the callback sequence
__declspec(allocate(".CRT$XLB")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_dyn_tls_init =
        reinterpret_cast<PIMAGE_TLS_CALLBACK>(__dyn_tls_init);

// End sentinel
__declspec(allocate(".CRT$XLZ")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_z = nullptr;

}

//===----------------------------------------------------------------------===//
// TLS directory
//
// The symbol name _tls_used is required by the linker to identify this.
// On x86, the symbol is decorated as __tls_used (extra underscore).
//===----------------------------------------------------------------------===//

// Force the linker to include the TLS directory structure.
// Without this, the linker may discard _tls_used if no TLS variables are used.
#if defined(_M_IX86) || defined(__i386__)
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:___xl_dyn_tls_init")
#else
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:__xl_dyn_tls_init")
#endif

#pragma section(".rdata$T", long, read)

extern "C" {

__declspec(allocate(".rdata$T")) __declspec(selectany)
    CRT_IMAGE_TLS_DIRECTORY _tls_used = {
        reinterpret_cast<uintptr_t>(&_tls_start),  // StartAddressOfRawData
        reinterpret_cast<uintptr_t>(&_tls_end),    // EndAddressOfRawData
        reinterpret_cast<uintptr_t>(&_tls_index),  // AddressOfIndex
        reinterpret_cast<uintptr_t>(&__xl_a),      // AddressOfCallBacks
        0,                                          // SizeOfZeroFill
        0                                           // Characteristics
};

}

#endif // _WIN32
