//===-- Thread-detach cleanup entry point -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __llvm_libc_thread_detach_cleanup: the single extern-C entry point that
// crt_tls.obj's `.CRT$XLC` callback invokes on DLL_THREAD_DETACH and
// DLL_PROCESS_DETACH. Owns the FaultGuard installation around the two
// destructor phases so that crt_tls.obj (linked into every consumer EXE)
// does NOT drag in LIBC_NAMESPACE-mangled internals (g_pcb, setjmp,
// tls_cleanup_run_all) as cross-module references.
//
// This file is compiled into c.dll but never into consumer images.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/macros/config.h"

// Defined by thread.cpp inside c.dll (weak default in
// cxa_thread_finalize_default.cpp). Runs C++ thread_local dtors + POSIX
// TSS callbacks for the current thread.
extern "C" void __cxa_thread_finalize(void *dso);

namespace {

// Mirror the loader's fdwReason constants without pulling <windows.h>.
constexpr unsigned long kDllProcessDetach = 0;
constexpr unsigned long kDllThreadDetach = 3;

} // namespace

// Called from crt_tls.obj's libc_tls_cleanup TLS callback. Runs INSIDE the
// loader lock — a faulting dtor would otherwise freeze every thread waiting
// on the loader; FAULT_GUARD_DTOR catches all HW faults except stack
// overflow (which recurses into VEH) and debugger traps.
extern "C" void __llvm_libc_thread_detach_cleanup(unsigned long reason) {
  if (reason != kDllThreadDetach && reason != kDllProcessDetach)
    return;

  using LIBC_NAMESPACE::windows::FaultGuard;
  using LIBC_NAMESPACE::windows::fault_guard_enter;
  using LIBC_NAMESPACE::windows::fault_guard_leave;
  using LIBC_NAMESPACE::windows::FAULT_GUARD_DTOR;

  {
    FaultGuard g;
    if (!fault_guard_enter(&g, FAULT_GUARD_DTOR)) {
      __cxa_thread_finalize(nullptr);
      fault_guard_leave(&g);
    }
  }
  {
    FaultGuard g;
    if (!fault_guard_enter(&g, FAULT_GUARD_DTOR)) {
      LIBC_NAMESPACE::internal::tls_cleanup_run_all();
      fault_guard_leave(&g);
    }
  }
}
