//===-- Thread-exit TLS cleanup for Windows --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Centralised thread-exit cleanup for libc subsystems using TLS slots.
//
// Each subsystem registers a (slot_index, callback) pair at init time.
// A .CRT$XLC TLS callback fires on DLL_THREAD_DETACH and walks the
// registry, reading each slot from the TEB and calling its cleanup
// function if the value is non-null.
//
// This replaces FLS callbacks entirely — no FlsAlloc, no ntdll FLS
// global state, no callback registration in kernel-managed tables.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_CLEANUP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_CLEANUP_H

#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup_state.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Register a TLS slot for thread-exit cleanup. Called at subsystem init.
// Registration is single-threaded (CRT init), but we use release-store on
// the count to ensure readers on other threads see fully-written entries.
LIBC_INLINE void tls_cleanup_register(DWORD index, TlsCleanupFn callback) {
  auto &state = g_pcb.tls_cleanup;
  unsigned slot = state.count.load(cpp::MemoryOrder::RELAXED);
  if (slot >= TLS_CLEANUP_MAX_SLOTS)
    return;
  state.entries[slot].tls_index = index;
  state.entries[slot].callback = callback;
  // Release-store publishes the entry to tls_cleanup_run_all() readers.
  state.count.store(slot + 1, cpp::MemoryOrder::RELEASE);
}

// Remove a TLS cleanup entry. Used by fork-child rollback paths that need to
// unwind a partially initialized subsystem before it ever becomes visible.
// Safe in the single-threaded fork child; normal runtime registration stays
// append-only.
LIBC_INLINE void tls_cleanup_unregister(DWORD index) {
  auto &state = g_pcb.tls_cleanup;
  unsigned count = state.count.load(cpp::MemoryOrder::RELAXED);
  for (unsigned i = 0; i < count; ++i) {
    if (state.entries[i].tls_index != index)
      continue;

    if (i + 1 != count)
      state.entries[i] = state.entries[count - 1];
    state.count.store(count - 1, cpp::MemoryOrder::RELEASE);
    return;
  }
}

// Called by the .CRT$XLC callback on thread exit.
// Out-of-line in c.dll so that crt_tls.obj (linked into every image)
// resolves this via the c.lib import lib rather than pulling g_pcb
// across DLL boundaries.
__attribute__((visibility("default"))) __declspec(dllexport)
void tls_cleanup_run_all();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_CLEANUP_H
