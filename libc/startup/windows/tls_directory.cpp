//===-- tls_directory.cpp - PE/COFF TLS directory and callbacks ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// IMAGE_TLS_DIRECTORY for PE/COFF thread-local storage. The Windows loader
// reads _tls_used to allocate per-thread TLS slots and run TLS callbacks
// (.CRT$XL* section) on thread attach/detach.
//
// Without this, thread_local variables silently fail — the loader has no
// TLS directory to process, so TLS slots are never allocated.
//
// TLS callbacks run inside the loader lock. Destructors must not call
// LoadLibrary/FreeLibrary or acquire locks that could deadlock.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

// Function pointer types for CRT callbacks.
using PVFV = void(__cdecl *)(void);
using PIMAGE_TLS_CALLBACK = void(__stdcall *)(void *, unsigned long, void *);

// IMAGE_TLS_DIRECTORY layout — pointer-sized fields differ by architecture.
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
struct ImageTlsDirectory {
  uint64_t StartAddressOfRawData;
  uint64_t EndAddressOfRawData;
  uint64_t AddressOfIndex;
  uint64_t AddressOfCallBacks;
  uint32_t SizeOfZeroFill;
  uint32_t Characteristics;
};
#else
struct ImageTlsDirectory {
  uint32_t StartAddressOfRawData;
  uint32_t EndAddressOfRawData;
  uint32_t AddressOfIndex;
  uint32_t AddressOfCallBacks;
  uint32_t SizeOfZeroFill;
  uint32_t Characteristics;
};
#endif

// TLS section boundaries. Linker merges .tls$* alphabetically.
#pragma section(".tls", long, read, write)
#pragma section(".tls$ZZZ", long, read, write)

extern "C" {

__declspec(allocate(".tls")) __declspec(selectany) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) __declspec(selectany) char _tls_end = 0;

// Loader writes the allocated TLS slot index here.
__declspec(selectany) unsigned long _tls_index = 0;

} // extern "C"

// TLS callback array. .CRT$XLA..XLZ are merged alphabetically.
// .CRT$XLAA initializes the security cookie before any other TLS callback.
// .CRT$XLB holds the dynamic TLS initializer for C++ thread_local variables.
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLAA", long, read)
#pragma section(".CRT$XLB", long, read)
#pragma section(".CRT$XLC", long, read)
#pragma section(".CRT$XLZ", long, read)

// Dynamic TLS initializer array (.CRT$XD*) — compiler emits per-variable
// init functions here for C++ thread_local variables with non-trivial ctors.
#pragma section(".CRT$XDA", long, read)
#pragma section(".CRT$XDZ", long, read)

extern "C" {

__declspec(allocate(".CRT$XDA")) __declspec(selectany) PVFV __xd_a[] = {
    nullptr};
__declspec(allocate(".CRT$XDZ")) __declspec(selectany) PVFV __xd_z[] = {
    nullptr};

} // extern "C"

// Security cookie init — must run before any /GS-protected function.
// Idempotent: repeated calls are no-ops once the cookie is initialized.
extern "C" void __security_init_cookie(void);

// __cxa_thread_finalize — runs thread_local destructors and POSIX TSS
// destructors for this thread. Idempotent: the second call is a no-op.
extern "C" void __cxa_thread_finalize(void *dso);

// Run libc TLS cleanup on thread/process detach.
//
// __cxa_thread_finalize is called here unconditionally so that foreign threads
// (raw NtCreateThreadEx, thread pool, COM callbacks) get their thread_local
// destructors and POSIX TSS destructors run on exit. For libc-created threads
// that already called __cxa_thread_finalize via lifecycle_cleanup (either
// manually or through tls_cleanup_run_all), this is a harmless no-op.
static void __stdcall libc_tls_cleanup(void *, unsigned long reason, void *) {
  constexpr unsigned long DLL_THREAD_DETACH_VAL = 3;
  constexpr unsigned long DLL_PROCESS_DETACH_VAL = 0;

  if (reason == DLL_THREAD_DETACH_VAL || reason == DLL_PROCESS_DETACH_VAL) {
    __cxa_thread_finalize(nullptr);
    LIBC_NAMESPACE::internal::tls_cleanup_run_all();
  }
}

// Initialize the security cookie on DLL_PROCESS_ATTACH. This is the first
// TLS callback (.CRT$XLAA) — runs before dyn_tls_init (.CRT$XLB), ensuring
// the /GS cookie is live before any function-pointer dispatch or non-trivial
// code executes. Only fires on process attach; thread attach does not need
// cookie re-init (the cookie is process-wide, not per-thread).
static void __stdcall cookie_tls_init(void *, unsigned long reason, void *) {
  constexpr unsigned long DLL_PROCESS_ATTACH_VAL = 1;
  if (reason == DLL_PROCESS_ATTACH_VAL)
    __security_init_cookie();
}

// Run dynamic TLS initializers on thread/process attach.
static void __stdcall dyn_tls_init(void *, unsigned long reason, void *) {
  constexpr unsigned long DLL_PROCESS_ATTACH_VAL = 1;
  constexpr unsigned long DLL_THREAD_ATTACH_VAL = 2;

  if (reason == DLL_THREAD_ATTACH_VAL || reason == DLL_PROCESS_ATTACH_VAL) {
    for (PVFV *fn = __xd_a; fn < __xd_z; ++fn) {
      if (*fn)
        (*fn)();
    }
  }
}

extern "C" {

__declspec(allocate(".CRT$XLA")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_a = nullptr;

__declspec(allocate(".CRT$XLAA")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_cookie_init =
        reinterpret_cast<PIMAGE_TLS_CALLBACK>(cookie_tls_init);

__declspec(allocate(".CRT$XLB")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_dyn_tls_init =
        reinterpret_cast<PIMAGE_TLS_CALLBACK>(dyn_tls_init);

__declspec(allocate(".CRT$XLC")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_libc_cleanup =
        reinterpret_cast<PIMAGE_TLS_CALLBACK>(libc_tls_cleanup);

__declspec(allocate(".CRT$XLZ")) __declspec(selectany)
    PIMAGE_TLS_CALLBACK __xl_z = nullptr;

} // extern "C"

// Force linker to include the TLS callback.
#if defined(__i386__)
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:___xl_dyn_tls_init")
#else
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:__xl_cookie_init")
#pragma comment(linker, "/INCLUDE:__xl_dyn_tls_init")
#pragma comment(linker, "/INCLUDE:__xl_libc_cleanup")
#endif

// IMAGE_TLS_DIRECTORY — the PE loader reads this to set up TLS.
#pragma section(".rdata$T", long, read)

extern "C" {

__declspec(allocate(".rdata$T")) __declspec(selectany)
    ImageTlsDirectory _tls_used = {
        reinterpret_cast<uintptr_t>(&_tls_start),
        reinterpret_cast<uintptr_t>(&_tls_end),
        reinterpret_cast<uintptr_t>(&_tls_index),
        reinterpret_cast<uintptr_t>(&__xl_a),
        0,
        0,
};

} // extern "C"
