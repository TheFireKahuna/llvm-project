//===-- .libcveh section registry for static VEH filter records -*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A COFF/PE section registry (built on SectionRegistry<Record>) that holds a
// VehFilter record for every subsystem that wants its filter installed into
// the unified VEH dispatch table during Tier B bring-up.
//
// Replaces the former dynamic register_veh_filter / unregister_veh_filter
// API. ALL VEH filters are now statically registered: drop a
// LIBC_REGISTER_VEH_FILTER call at file scope in the filter's owning TU.
// `register_all_static_veh_filters()` sweeps the section once during
// Tier A (Phase 0d) — before pcb_seal_readonly_a() — and inserts every
// record into the dispatch table via insert_static_veh_filter().
//
// After Tier A, the filter table lives in PAGE_READONLY Zone 0 memory:
// hardware-immutable, no synchronization needed for reads, and the
// master handler is a pure function of immutable inputs. Subsystem-
// specific runtime gating (e.g. signal_veh_transport returning
// CONTINUE_SEARCH when no SEH-class handler is set) is the filter's
// own responsibility.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_FILTER_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_FILTER_REGISTRY_H

#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/OSUtil/windows/veh/veh_state.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Walk `.libcveh$M` and install every record into the VEH dispatch table via
// insert_static_veh_filter(). Called once from Tier A Phase 0d (in
// __libc_bootstrap), before pcb_seal_readonly_a() seals the table into
// PAGE_READONLY memory. Single-threaded at this point.
//
// A readonly-section audit is performed at walker entry — a downstream TU
// that unioned MEM_WRITE into `.libcveh$M` via a duplicate
// `#pragma section` would corrupt the filter table.
void register_all_static_veh_filters();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Register a VEH filter into `.libcveh$M`. `tag` must be unique across the
// entire link (duplicates produce a linker duplicate-symbol error).
//
// Emits two artifacts:
//   1. The section record itself (consumed by register_all_static_veh_filters).
//   2. A trivially-defined anchor function `__libc_veh_anchor_<tag>` that
//      veh_filter_registry.cpp's `anchor_all_veh_filters` references by name.
//      Static-archive consumers (libc.lib in hermetic tests) only pull a
//      .obj in when something it defines is referenced from outside; without
//      this anchor, a TU whose only outward artifact is the section record
//      would be silently dropped, taking the filter registration with it.
//      The anchor list in veh_filter_registry.cpp is the single point where
//      a missing filter-TU link surfaces as an unresolved-symbol error
//      instead of a runtime "filter never fired".
//
// Must expand at namespace scope (file scope, outside any namespace {}
// block).
#define LIBC_REGISTER_VEH_FILTER(tag, mask, handler_fn, prio)                  \
  LIBC_SECTION_REGISTER(libcveh, ::LIBC_NAMESPACE::windows::VehFilter, tag,    \
                        {(mask), (handler_fn), (prio)})                        \
  extern "C" [[gnu::used]] void __libc_veh_anchor_##tag(void) {}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_FILTER_REGISTRY_H
