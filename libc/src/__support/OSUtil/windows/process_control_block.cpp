//===-- Process Control Block PE section instantiation -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Instantiates the process-wide ProcessControlBlock in a dedicated PE section.
//
// The ".pcb" section has IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE |
// IMAGE_SCN_CNT_INITIALIZED_DATA. The linker emits it with VirtualSize =
// sizeof(ProcessControlBlock) and SizeOfRawData = 0 (all-zero initializer
// needs no file backing). The NT loader demand-zero-fills pages on first
// touch — no runtime page_reserve or NtCreateSection needed.
//
// After CRT init completes, pcb_seal_readonly_a() protects page 0 and
// pcb_seal_readonly_b() protects page 1 as PAGE_READONLY. Page 0 holds
// lifetime-immutable PCB constants and the sealed VEH dispatch table;
// page 1 (Zone 0b) holds fork-mutable state (pid, security cookie,
// dll_notify_cookie). Page 0 is never unsealed; page 1 is briefly
// unsealed during libc_fork_reinit() and veh_core fini.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process_control_block.h"

#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block_access.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// =========================================================================
// PE section placement
//
// The ".pcb" section is a dedicated PE section for the ProcessControlBlock.
// - `read, write` → IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE
// - `long` → IMAGE_SCN_CNT_INITIALIZED_DATA (ensures the section is
//   included in the PE even though SizeOfRawData is 0)
//
// __LIBC_SECTION_ATTR(".pcb") places g_pcb in this section. The `= {}`
// zero-initializer means the linker can emit SizeOfRawData = 0 (pure BSS).
//
// The NT loader maps the section and demand-zero-fills pages. This is
// identical to how .bss works but in a dedicated section, giving us:
// - Spatial locality: all PCB fields in contiguous pages
// - Debugger visibility: ".pcb" appears in PE section dumps
// - Protection boundary: page 0 can be sealed PAGE_READONLY independently
// =========================================================================

// Section flags are derived from `g_pcb`'s type: non-const
// ProcessControlBlock → clang registers `.pcb` as PSF_Read|PSF_Write,
// which matches the intent (runtime-mutable until the Tier A zone seals
// protect page 0 / page 0b via NtProtectVirtualMemory). lld-link emits
// the section with VirtualSize = sizeof(g_pcb) and SizeOfRawData = 0
// for the zero-initialized global.

// constinit: guarantees the zero-initializer is resolved at compile time.
// Prevents dynamic-init-ordering bugs if a field type ever gains a non-trivial
// constructor. All PCB field types (scalars, cpp::Atomic, RawMutex) have
// constexpr default/value constructors, so this is satisfied today and will
// break loudly if that invariant is violated in the future.
// C++20 constinit equivalent for C++17; Clang-only (NTPOSIX target).
//
// alignas(4096) is repeated on the instance: ProcessControlBlock already
// carries alignas(4096) on the struct, but lld-link's section-alignment
// propagation from a typedef'd alignment is uneven across linker versions.
// Tagging the variable with the same alignas guarantees the
// IMAGE_SCN_ALIGN_4096BYTES section header bit is set on `.pcb`, which is
// what the loader honours when mapping pages — and what makes the
// pcb_seal_readonly_a/b NtProtectVirtualMemory calls land on a page boundary.
[[clang::require_constant_initialization]] alignas(4096)
    __LIBC_SECTION_ATTR(".pcb") ProcessControlBlock g_pcb = {};

// =========================================================================
// Layout and size validation
// =========================================================================

// Total PCB size guard — catch unexpected growth from field additions.
static_assert(sizeof(ProcessControlBlock) <= 65536,
              "PCB exceeds 64 KB — review field placement and consider "
              "moving large arrays to external demand-commit storage");

// =========================================================================
// Protection zones
//
// Zone 0  (page 0)  — sealed PAGE_READONLY at end of Tier A; never unsealed.
//                     Holds PCB constants + sealed VEH dispatch table.
// Zone 0b (page 1)  — sealed PAGE_READONLY at end of Tier A; unsealed only
//                     during libc_fork_reinit() and veh_core fini to update
//                     pid / cookies / dll_notify_cookie.
// Zone 1+ (page 2+) — always PAGE_READWRITE.
//
// Each seal/unseal targets exactly one page via NtProtectVirtualMemory.
//
// g_zone0b_unseal_depth is a debug-mode witness for "Zone 0b is currently
// writable". Production code does not consult it — the protection state is
// enforced by the page table — but PcbInitAccess Zone 0b setters assert
// against it so a stray write outside an unseal/seal pair fails loudly in
// debug builds rather than silently AV'ing in production. The depth is
// process-wide because the unseal/reseal pairs in libc_fork_reinit and
// veh_core_fini run single-threaded; we still use ACQ_REL ordering so the
// counter ordering can be reasoned about under unexpected concurrency.
namespace {
cpp::Atomic<int> g_zone0b_unseal_depth{0};
} // namespace

bool zone0b_writable_now() {
  if (pcb_init_state() < PcbInitState::TierA)
    return true; // pre-seal: pages are still PAGE_READWRITE.
  return g_zone0b_unseal_depth.load(cpp::MemoryOrder::ACQUIRE) > 0;
}

void pcb_init_state_advance_checked(PcbInitState expected,
                                    PcbInitState next) {
  // Monotonic advance. A mismatch means the bootstrap caller skipped or
  // reordered a phase — terminate with STATUS_DLL_INIT_FAILED so the
  // failure is visible in WER dumps rather than manifesting as a
  // downstream assertion fire or, worse, silently running with an
  // uninitialised invariant. Atomic CAS (single-threaded context but
  // costs one ACQ_REL on a warm line).
  auto current = static_cast<uint32_t>(expected);
  if (!g_pcb.init_state.compare_exchange_strong(current,
                                                static_cast<uint32_t>(next),
                                                cpp::MemoryOrder::ACQ_REL,
                                                cpp::MemoryOrder::RELAXED)) {
    ::NtTerminateProcess(NtCurrentProcess(),
                         static_cast<NTSTATUS>(0xC0000142L));
    __builtin_unreachable();
  }
}

bool pcb_seal_readonly_a() {
  if (g_pcb.zone0.page_size() == 0)
    return false;
  return internal::page_protect(&g_pcb.zone0, g_pcb.zone0.page_size(),
                                PAGE_READONLY);
}

bool pcb_seal_readonly_b() {
  if (g_pcb.zone0.page_size() == 0)
    return false;
  bool ok = internal::page_protect(&g_pcb.zone0b, g_pcb.zone0.page_size(),
                                   PAGE_READONLY);
  if (ok) {
    // Decrement only on success so a failed reseal leaves the counter
    // matching the actual writable state.
    g_zone0b_unseal_depth.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  }
  return ok;
}

bool pcb_unseal_readonly_b() {
  if (g_pcb.zone0.page_size() == 0)
    return false;
  bool ok = internal::page_protect(&g_pcb.zone0b, g_pcb.zone0.page_size(),
                                   PAGE_READWRITE);
  if (ok)
    g_zone0b_unseal_depth.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  return ok;
}

namespace internal {

uint32_t pcb_page_size() { return g_pcb.zone0.page_size(); }

uint32_t pcb_alloc_granularity() { return g_pcb.zone0.alloc_granularity(); }

void *pcb_min_address() { return g_pcb.zone0.min_address(); }

void *pcb_max_address() { return g_pcb.zone0.max_address(); }

TlsCleanupState &pcb_tls_cleanup() { return g_pcb.tls_cleanup; }

} // namespace internal

} // namespace LIBC_NAMESPACE_DECL

// =========================================================================
// TLS cleanup — out-of-line so crt_tls.obj doesn't reference g_pcb directly
// =========================================================================

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Walk the TLS cleanup table in DESCENDING phase order, then within a
// phase in REVERSE registration order. The phase ordering is load-
// bearing: lifecycle (Phase 4) retires the dying thread's lifecycle
// into the Crystalline scratch region living in the substrate (Phase 1)
// arena, so substrate teardown must run AFTER lifecycle teardown — see
// tls_cleanup_state.h.
//
// We iterate ONLY the defined phases (Lifecycle → WaitSlot → Allocator
// → Substrate) — 4 outer iterations, not 256. Registration enforces
// `phase ∈ {1,2,3,4}` via __builtin_trap so unrecognized values are
// caught at init, not silently dropped at exit. Total work per thread
// exit: 4 × TLS_CLEANUP_MAX_SLOTS (=16) = 64 compare-and-skip
// iterations, dominated by the cleanup callbacks themselves.
void tls_cleanup_run_all() {
  auto &state = g_pcb.tls_cleanup;
  unsigned count = state.count.load(cpp::MemoryOrder::ACQUIRE);
  if (count > TLS_CLEANUP_MAX_SLOTS)
    count = TLS_CLEANUP_MAX_SLOTS;

  // Defined phases in DESCENDING order. Adding a new phase requires
  // adding both the constant in tls_cleanup_state.h, the registration-
  // time check in tls_cleanup_register, AND an entry in this list at
  // its correct ordering position.
  constexpr uint8_t kDescendingPhases[] = {
      kTlsCleanupPhaseLifecycle, // 4: lifecycle, signal, IoRing, etc.
      kTlsCleanupPhaseWaitSlot,  // 3: futex parking lot.
      kTlsCleanupPhaseAllocator, // 2: slab pools, posix allocator.
      kTlsCleanupPhaseSubstrate, // 1: ThreadScratch arena. LAST.
  };

  for (uint8_t phase : kDescendingPhases) {
    for (unsigned ridx = count; ridx > 0; --ridx) {
      unsigned i = ridx - 1;
      if (state.entries[i].phase != phase)
        continue;
      void *val = teb_tls_get(state.entries[i].tls_index);
      if (val) {
        state.entries[i].callback(val);
        teb_tls_set(state.entries[i].tls_index, nullptr);
      }
    }
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

