//===-- tls_cleanup_callback.cpp - .CRT$XLC slot for thread-detach ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single .CRT$XLC TLS callback that hands DLL_THREAD_DETACH /
// DLL_PROCESS_DETACH off to c.dll's __llvm_libc_thread_detach_cleanup.
//
// This TU is compiled into a standalone startup object (crt_tls_cleanup.obj)
// and linked into the consumer EXE only — never into c.dll. c.dll already
// has its own TLS infrastructure via tls_directory.cpp / crt_tls.obj
// (_tls_index, IMAGE_TLS_DIRECTORY, cookie init, dyn TLS init). Adding a
// second .CRT$XLC slot in c.dll would cause LdrShutdownThread's module walk
// to fire __llvm_libc_thread_detach_cleanup twice per external thread exit.
// Splitting the cleanup registration out makes single-firing structurally
// enforced by linker geometry rather than a runtime guard.
//
// Stays header-light on purpose — the only cross-module reference is the
// extern-C symbol below, so this object carries no LIBC_NAMESPACE-mangled
// dependencies into consumer EXEs.
//
//===----------------------------------------------------------------------===//

#include "include/__llvm-libc-common.h"
#include "src/__support/macros/config.h"

// Function pointer type for CRT TLS callbacks. Pinned to MS x64 ABI via
// LIBC_MSABI; the loader invokes the callback under the MS ABI regardless
// of the TU default (NT-POSIX defaults to SysV). Derived via decltype on
// an unreferenced external prototype because LIBC_MSABI can only decorate
// a function declaration, not a function-type alias.
LIBC_MSABI void __image_tls_callback_type_source(void *, unsigned long,
                                                 void *);
using PIMAGE_TLS_CALLBACK = decltype(&__image_tls_callback_type_source);

// Defined in c.dll (libc/src/__support/OSUtil/windows/tls/tls_thread_detach.cpp).
// Runs FaultGuard-wrapped __cxa_thread_finalize + tls_cleanup_run_all().
extern "C" void __llvm_libc_thread_detach_cleanup(unsigned long reason);

// Thin dispatcher — keeps this TU free of LIBC_NAMESPACE internals.
LIBC_MSABI static void libc_tls_cleanup(void *, unsigned long reason, void *) {
  __llvm_libc_thread_detach_cleanup(reason);
}

extern "C" {

// .CRT$XLC sits between .CRT$XLB (dyn TLS init, owned by tls_directory.cpp)
// and .CRT$XLZ (sentinel, owned by tls_directory.cpp). The linker merges
// .CRT$XL* alphabetically across all TUs in the link.
[[gnu::retain]] __LIBC_SECTION_ATTR(".CRT$XLC") __LIBC_SELECTANY_ATTR extern const
    PIMAGE_TLS_CALLBACK __xl_libc_cleanup = libc_tls_cleanup;

} // extern "C"
