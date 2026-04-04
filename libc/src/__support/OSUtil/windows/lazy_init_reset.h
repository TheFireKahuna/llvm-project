//===-- .libclzr section registry for exec lazy-init reset -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A COFF/PE section registry (built on SectionRegistry<Record>) that holds
// a reset-thunk pointer for every LazyInit<> that wants to be cleared to
// kUninitialized when exec_self_hollow() swaps the process image.
//
// Why central instead of per-subsystem explicit resets: each subsystem
// already has an `xxx_exec_reinit()` (or a generalized `alloc_exec_reinit()`)
// hook for tearing down handles and other irreversible state. The LazyInit
// state_ atomic is a separate concern — it has to become kUninitialized so
// the next user-facing call re-runs InitFn after exec. Doing that from one
// place (this walker) removes the boilerplate "g_x_init.reset_for_fork();"
// line from every exec hook and makes it impossible to forget.
//
// Subsystems that are pure cached data (e.g. NLS locale blob derived from
// NtInitializeNlsFiles — same kernel blob survives exec) do NOT register
// here; their LazyInit stays ready across exec. Subsystems that hold
// slab-pool state, TLS slot indices, or any kernel-object reservation
// that must be re-acquired in the new image DO register.
//
// Usage at the LazyInit definition site:
//
//   namespace LIBC_NAMESPACE {
//   namespace internal {
//   static int foo_init_impl() { ... }
//   LazyInit<&foo_init_impl> g_foo_init;
//   } // namespace internal
//   } // namespace LIBC_NAMESPACE
//
//   LIBC_REGISTER_LAZY_RESET(foo,
//                            ::LIBC_NAMESPACE::internal::g_foo_init)
//
// The macro must expand at file scope (outside any namespace {} block);
// it opens its own LIBC_NAMESPACE::internal namespace for the thunk and
// the section record.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LAZY_INIT_RESET_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LAZY_INIT_RESET_H

#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// One record per registered LazyInit<>. The thunk closes over the
// concrete template instantiation so the walker doesn't need to know
// any types.
struct LazyInitResetEntry {
  void (*reset)();
};

// Walk `.libclzr$M` and invoke each reset thunk. Called from
// exec_self_hollow() Phase 5 after per-subsystem exec_reinit hooks.
// Runs single-threaded (all non-current threads are frozen).
//
// Tier B audit: the section must be read-only. A downstream TU that
// wrote `#pragma section(".libclzr$M", read, write)` would union
// MEM_WRITE into the merged section — fatal invariant violation.
// verify_section_readonly() traps in debug; release builds trust Tier B
// caught it.
void reset_all_lazy_inits();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Register a LazyInit<> reset thunk into `.libclzr$M`. `tag` must
// be unique across the link for this registry; it becomes part of the
// record variable's strong symbol name (duplicate-tag => linker error).
// `lazy_init_var` is the fully-qualified name of the LazyInit<> instance
// to reset.
//
// Must expand at namespace scope (file scope, outside any namespace {}
// block).
#define LIBC_REGISTER_LAZY_RESET(tag, lazy_init_var)                           \
  namespace LIBC_NAMESPACE_DECL {                                              \
  namespace internal {                                                         \
  static void libc_lazyreset_thunk_##tag() { (lazy_init_var).reset(); }        \
  }                                                                            \
  }                                                                            \
  LIBC_SECTION_REGISTER(                                                       \
      libclzr,                                                           \
      ::LIBC_NAMESPACE::internal::LazyInitResetEntry,                          \
      tag,                                                                     \
      {&::LIBC_NAMESPACE::internal::libc_lazyreset_thunk_##tag})               \
  extern "C" [[gnu::used]] void __libc_lzr_anchor_##tag(void) {}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LAZY_INIT_RESET_H
