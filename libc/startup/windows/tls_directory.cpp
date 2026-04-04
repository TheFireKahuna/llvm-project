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

#include "include/__llvm-libc-common.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

// Function pointer types for CRT callbacks. PIMAGE_TLS_CALLBACK is invoked
// by the PE loader with MS x64 ABI; LIBC_MSABI pins the calling convention
// regardless of the TU default. Derived via `decltype` on an unreferenced
// external prototype — LIBC_MSABI can only decorate a function declaration,
// not a function-type alias (trailing-alias form trips `-Wgcc-compat`).
// PVFV is called by our own startup walker (not the kernel), so it
// inherits the TU default.
LIBC_MSABI void __image_tls_callback_type_source(void *, unsigned long,
                                                 void *);
using PIMAGE_TLS_CALLBACK = decltype(&__image_tls_callback_type_source);
using PVFV = void(*)(void);

// IMAGE_TLS_DIRECTORY layout. The four address fields are stored as
// native pointers rather than integer types so the initializer below
// can be constant-evaluated (taking the address of a global is a
// constexpr core-constant expression; reinterpret_cast to uintptr_t is
// not). On x64/ARM64 a pointer is 8 bytes (matching the DDK's
// ULONGLONG layout); on x86/ARM it's 4 bytes (matching ULONG). Either
// way the on-disk layout the PE loader consumes is unchanged.
struct ImageTlsDirectory {
  const void *StartAddressOfRawData;
  const void *EndAddressOfRawData;
  const void *AddressOfIndex;
  const void *AddressOfCallBacks;
  uint32_t SizeOfZeroFill;
  uint32_t Characteristics;
};

// TLS section boundaries. Linker merges .tls$* alphabetically. Section
// flags come from the variable types: `_tls_start` / `_tls_end` are
// non-const `char`, which registers `.tls` / `.tls$ZZZ` as R/W —
// matching the mutable TLS template data the loader copies per-thread.
extern "C" {

__LIBC_SECTION_ATTR(".tls") __LIBC_SELECTANY_ATTR char _tls_start = 0;
__LIBC_SECTION_ATTR(".tls$ZZZ") __LIBC_SELECTANY_ATTR char _tls_end = 0;

// Loader writes the allocated TLS slot index here.
__LIBC_SELECTANY_ATTR unsigned long _tls_index = 0;

} // extern "C"

// TLS callback array. .CRT$XLA..XLZ are merged alphabetically.
// .CRT$XLAA initializes the security cookie before any other TLS callback.
// .CRT$XLB holds the dynamic TLS initializer for C++ thread_local variables.
// Callback slots below are `extern const PIMAGE_TLS_CALLBACK`, so clang
// registers .CRT$XL* as read-only without any #pragma section.
//
// Dynamic TLS initializer array (.CRT$XD*) — compiler emits per-variable
// init functions here for C++ thread_local variables with non-trivial
// ctors. Bookends are `extern const PVFV`, driving PSF_Read.

extern "C" {

__LIBC_SECTION_ATTR(".CRT$XDA") __LIBC_SELECTANY_ATTR extern const PVFV __xd_a[] = {
    nullptr};
__LIBC_SECTION_ATTR(".CRT$XDZ") __LIBC_SELECTANY_ATTR extern const PVFV __xd_z[] = {
    nullptr};

} // extern "C"

// Security cookie init — must run before any /GS-protected function.
// Idempotent: repeated calls are no-ops once the cookie is initialized.
extern "C" void __security_init_cookie(void);

// Note: the .CRT$XLC thread-detach cleanup callback that calls
// __llvm_libc_thread_detach_cleanup lives in tls_cleanup_callback.cpp
// (compiled as a separate startup object crt_tls_cleanup.obj). It is linked
// into the consumer EXE only — c.dll never carries it, so LdrShutdownThread
// fires the cleanup exactly once per external thread exit (from the EXE's
// .CRT$XLC slot) instead of once per loaded module. Splitting the
// registration out enforces single-firing by linker geometry.

// Initialize the security cookie on DLL_PROCESS_ATTACH. This is the first
// TLS callback (.CRT$XLAA) — runs before dyn_tls_init (.CRT$XLB), ensuring
// the /GS cookie is live before any function-pointer dispatch or non-trivial
// code executes. Only fires on process attach; thread attach does not need
// cookie re-init (the cookie is process-wide, not per-thread).
LIBC_MSABI static void cookie_tls_init(void *, unsigned long reason, void *) {
  constexpr unsigned long DLL_PROCESS_ATTACH_VAL = 1;
  if (reason == DLL_PROCESS_ATTACH_VAL)
    __security_init_cookie();
}

// Run dynamic TLS initializers on thread/process attach.
LIBC_MSABI static void dyn_tls_init(void *, unsigned long reason, void *) {
  constexpr unsigned long DLL_PROCESS_ATTACH_VAL = 1;
  constexpr unsigned long DLL_THREAD_ATTACH_VAL = 2;

  if (reason == DLL_THREAD_ATTACH_VAL || reason == DLL_PROCESS_ATTACH_VAL) {
    for (const PVFV *fn = __xd_a; fn < __xd_z; ++fn) {
      if (*fn)
        (*fn)();
    }
  }
}

extern "C" {

__LIBC_SECTION_ATTR(".CRT$XLA") __LIBC_SELECTANY_ATTR extern const
    PIMAGE_TLS_CALLBACK __xl_a = nullptr;

[[gnu::retain]] __LIBC_SECTION_ATTR(".CRT$XLAA") __LIBC_SELECTANY_ATTR extern const
    PIMAGE_TLS_CALLBACK __xl_cookie_init = cookie_tls_init;

[[gnu::retain]] __LIBC_SECTION_ATTR(".CRT$XLB") __LIBC_SELECTANY_ATTR extern const
    PIMAGE_TLS_CALLBACK __xl_dyn_tls_init = dyn_tls_init;

// .CRT$XLC (libc_tls_cleanup) lives in tls_cleanup_callback.cpp — linked
// into the consumer EXE only, so the cleanup fires exactly once per
// external thread exit (from the EXE's slot) instead of once per loaded
// module that carries crt_tls.obj.

__LIBC_SECTION_ATTR(".CRT$XLZ") __LIBC_SELECTANY_ATTR extern const
    PIMAGE_TLS_CALLBACK __xl_z = nullptr;

} // extern "C"

// IMAGE_TLS_DIRECTORY — the PE loader reads this to set up TLS.
// `_tls_used` is `extern const`, so clang registers `.rdata$T` as
// read-only automatically. `[[gnu::retain]]` emits /INCLUDE: into
// `.drectve` so the PE loader's TLS directory entry is never stripped.
extern "C" {

[[gnu::retain]] __LIBC_SECTION_ATTR(".rdata$T") __LIBC_SELECTANY_ATTR extern const
    ImageTlsDirectory _tls_used = {
        &_tls_start,
        &_tls_end,
        &_tls_index,
        &__xl_a,
        0,
        0,
};

} // extern "C"
