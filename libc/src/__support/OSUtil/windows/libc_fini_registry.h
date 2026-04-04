//===-- .libcfin section registry for FreeLibrary teardown -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Phased COFF/PE section registry that holds a teardown thunk for every
// subsystem that owns reclaimable state at FreeLibrary time. Walked once
// in reverse phase order from `__libc_dll_fini()`.
//
// Why a registry rather than an explicit reverse-of-init list:
//
//   - Registration lives next to the function definition. A subsystem
//     that adds a new fini cannot forget to wire it into __libc_dll_fini
//     — the LIBC_REGISTER_FINI is a co-located declaration, not a
//     separate edit in a central file.
//
//   - The previous explicit list silently dropped 8 of 16 finis because
//     the central file was edited piecemeal as wrappers were added. The
//     section makes "exists" and "wired" the same property by
//     construction.
//
//   - Teardown ordering is coarse (a handful of phase buckets), not
//     fine-grained — `void(*)()` thunks suffice. Within a phase
//     bucket, finis must be order-independent. The contract every
//     subsystem follows (already documented at libc_subsystem_init.cpp)
//     is "fini is a no-op if init didn't run", which composes cleanly
//     with intra-bucket ordering being undefined.
//
// Phase assignments (highest dies first; reverse iteration walks $P9..$P0):
//
//   $P9 — alpc_bus            : close inbound port; no further requests
//                                can reach signal/lock_table handlers.
//   $P8 — signal, lock_table  : tear down own ALPC ports + state. The
//                                bus-side handler dispatch table is
//                                c.dll image memory, reclaimed by DLL
//                                unmap; no per-handler unregister.
//   $P7 — reactor             : drain thread services every other
//                                subsystem's IOCP completions; must
//                                outlive them.
//   $P6 — fd_table            : close every live fd. NtClose drops
//                                kernel byte-range locks, so this also
//                                covers locks the reactor-driven
//                                lock_table missed.
//   $P5 — mlock_policy        : VEH filter is sealed in Zone 0 and
//                                cannot be removed; this clears the
//                                policy state the filter consults.
//                                The filter self-gates on
//                                `onfault_state_constructed` so faults
//                                arriving between this fini and the
//                                $P0 master-VEH teardown fall through
//                                to the next filter instead of reading
//                                destructed state.
//   $P4 — pools               : ofd_pool, file_pool, wait_slot,
//                                thread_storage, named_semaphore,
//                                mapping_table. Independent SlabPool /
//                                radix-tree teardowns.
//   $P3 — lifecycle           : every other subsystem reads
//                                get_current_lifecycle() during its
//                                own fini; lifecycle TLS slot must
//                                outlive all of them.
//   $P2 — posix_alloc         : allocator base. Anything that calls
//                                free() during its destructor must
//                                have already run.
//   $P1 — va_inventory        : drop LdrRegisterDllNotification cookie
//                                so a late LOAD/UNLOAD does not trap
//                                into freed code.
//   $P0 — veh_core            : remove the master VEH and DLL-load
//                                notification last. After this, any
//                                fault is undefined behaviour and the
//                                loader is about to unmap the image
//                                anyway.
//
// Within a bucket the linker merge order is undefined; that is fine
// because each bucket's members are documented above as
// order-independent.
//
// Usage at the fini definition site (in the subsystem's own TU):
//
//   namespace LIBC_NAMESPACE {
//   namespace alpc_bus {
//   void fini() { ... }
//   } // namespace alpc_bus
//   } // namespace LIBC_NAMESPACE
//
//   LIBC_REGISTER_FINI(9, alpc_bus, &::LIBC_NAMESPACE::alpc_bus::fini)
//
// The macro must expand at file scope (outside any namespace {} block);
// it opens its own LIBC_NAMESPACE::internal namespace for the record.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_FINI_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_FINI_REGISTRY_H

#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// One record per registered fini. The thunk closes over the concrete
// teardown function so the walker doesn't need to know any types.
struct FiniEntry {
  void (*fini)();
};

// Walk `.libcfin$P9..$P0` in reverse phase order, invoking each thunk.
// Called once from `__libc_dll_fini()` after the init gate is poisoned.
// Runs single-threaded by loader contract: DLL_PROCESS_DETACH holds the
// loader lock and no thread that the libc spawned (reactor drain) is
// permitted to outlive the corresponding fini call (reactor's own fini
// joins it).
//
// Per-thunk invariants — every fini MUST satisfy both:
//   - No slab allocation. SlabPool teardown happens at $P4 (pools); any
//     malloc/free issued from a $P3 or earlier fini hits a destructed
//     pool and AVs. posix_alloc itself is gone after $P2.
//   - No futex / WaitSlot acquisition. wait_slot is destroyed at the
//     same $P4 bucket; a Futex::wait on a torn-down WaitSlot pool walks
//     a freed Treiber stack and AVs. RawMutex / CndVar / Barrier all
//     ride on Futex — none are safe to take during fini.
// Subsystems that own resources requiring blocking cleanup must drain
// them before returning from a higher-numbered phase.
//
// Like the .libclzr walker, a Tier-B-equivalent readonly audit runs at
// entry: a downstream TU that wrote `#pragma section(".libcfin$P5",
// read, write)` would union MEM_WRITE into the merged section — fatal
// invariant violation, caught here in debug builds.
void run_all_finis();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Register a fini thunk into `.libcfin$P<phase>`. `phase` must resolve
// to a literal digit 0..9 (or a #define expanding to one); see the
// LIBC_SECTION_REGISTER_PHASED docs in section_registry.h for the
// preprocessor contract.
//
// `tag` must be unique across the link for this registry; it becomes
// part of the record variable's strong symbol name (duplicate-tag =>
// linker error). Use the subsystem name (`alpc_bus`, `reactor`, …).
//
// `fn_ptr` is a `void(*)()` taken with `&` from the subsystem's own
// teardown function. The function may live in any namespace; the macro
// only stores the pointer.
//
// Must expand at namespace scope (file scope, outside any namespace {}
// block).
#define LIBC_REGISTER_FINI(phase, tag, fn_ptr)                                 \
  LIBC_SECTION_REGISTER_PHASED(libcfin,                                        \
                               ::LIBC_NAMESPACE::internal::FiniEntry, phase,   \
                               tag, {(fn_ptr)})                                \
  extern "C" [[gnu::used]] void __libc_fin_anchor_##tag(void) {}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_FINI_REGISTRY_H
