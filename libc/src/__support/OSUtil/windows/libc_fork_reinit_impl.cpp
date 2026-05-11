//===-- Fork-child reinit walker ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// libc_fork_reinit() runs in the fork child after NtCreateProcessEx, before
// returning to user code. The child is single-threaded when this runs — no
// lock contention, no races.
//
// Dispatch is via the `.libcfork$M` section registry: each subsystem
// declares its reinit hook with LIBC_REGISTER_FORK_REINIT(tag, priority,
// &fn) (see libc_fork_registry.h). The walker collects all live entries,
// sorts by priority, and invokes them. Subsystems absent from the per-test
// trimmed libc.lib have no record in the section, so the walker simply
// doesn't visit them — no per-test CMake plumbing, no link-time complaints
// about subsystems the test doesn't use.
//
// Defensive properties carried over from the explicit-call dispatcher:
//
//   * PCB Zone 0b is unsealed for exactly the duration of this function
//     (fields rewritten: pid, parent_pid, security_cookie, complement,
//     zone_canary, dll_notify_cookie).
//   * Inherited canary is validated BEFORE rotation so parent-side
//     corruption is caught rather than masked by the fresh seed.
//   * `.libcfork` is audited PAGE_READONLY before the walker dereferences
//     any function pointer — same `enforce_section_readonly_or_fastfail`
//     tripwire as `.libcveh` (an arbitrary-write gadget that flipped the
//     section to RW would be caught here, before the indirect call lets
//     it become a control-flow primitive).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_lock.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/OSUtil/windows/tls/teb_fixup.h"
#include "src/__support/macros/config.h"

LIBC_DEFINE_SECTION_BOOKENDS(libcfork,
                             ::LIBC_NAMESPACE::internal::ForkReinitEntry)

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Rotate the PCB Zone 0 security cookie + complement + zone canary as a
// consistent set. This is the libc-internal cookie used by the Zone 0/1
// canary mechanism (see pcb_check_canary), NOT the compiler-managed /GS
// cookie `__security_cookie`.
//
// /GS reseed is intentionally skipped on fork: every caller frame on the
// stack has saved a canary derived from the parent's cookie, and changing
// __security_cookie mid-flow would crash the child on the first epilogue
// return. glibc and musl handle this the same way — the stack cookie is
// COW-shared with the parent, and rotation only happens at exec when the
// caller frames are torn down. The PCB cookie has no such caller-frame
// dependence and is safe to rotate here.
//
// Source: ProcessPrng (CSPRNG, always succeeds on Win10 20H2+). The Zone 0
// page is unsealed by the caller and resealed at the end of libc_fork_reinit.
[[gnu::no_stack_protector]]
static void rotate_pcb_security_cookie() {
  uintptr_t fresh = 0;
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&fresh), sizeof(fresh));
  // Avoid the documented "default" sentinel value collision so canary
  // checks remain a strict equality test.
  if (fresh == 0)
    fresh = 1;
  internal::PcbInitAccess::set_security_cookie(fresh);
  internal::PcbInitAccess::set_security_cookie_complement(~fresh);
  internal::PcbInitAccess::init_canary();
}

// Bound on the number of fork-reinit entries the walker is willing to
// dispatch. Generous headroom over today's ~37 (the entire current call
// list under section-registry conversion); a build-time exceedance trips
// the assert below before any indirect call. Sized so the on-stack copy
// stays under one page (64 * 24 B = 1.5 KiB).
static constexpr size_t kMaxForkReinitEntries = 64;

// Stable insertion sort by priority. N is small (~37 today, capped at
// kMaxForkReinitEntries above) so O(N^2) is fine; stability matters
// because equal-priority entries keep their relative declaration order
// (mirroring the explicit-call dispatcher's source-order semantics for
// "ordering is irrelevant within band").
[[gnu::no_stack_protector]]
static void sort_by_priority(ForkReinitEntry *arr, size_t n) {
  for (size_t i = 1; i < n; ++i) {
    ForkReinitEntry key = arr[i];
    size_t j = i;
    while (j > 0 && arr[j - 1].priority > key.priority) {
      arr[j] = arr[j - 1];
      --j;
    }
    arr[j] = key;
  }
}

// No top-level idempotency guard: every candidate witness (static
// atomic, thread_local) is COW-inherited via fork.cpp's
// copy_parent_teb_state, and PID keying has a multi-generation
// reuse hole. The legit "child-atfork-callback fork()" case is
// sequential (grandchild gets its own fresh reinit), not nested.
// Each subsystem's xxx_fork_reinit() is independently idempotent.
void libc_fork_reinit() {
  // =====================================================================
  // APC delivery during the Zone 0b unseal window
  //
  // User-mode APCs queued via NtQueueApcThread fire only when the target
  // thread enters an alertable wait (NtWaitForSingleObject Alertable=TRUE,
  // SleepEx, etc.) or explicitly calls NtTestAlert. Special user APCs
  // (NtQueueApcThreadEx2 with QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC) are
  // the one exception: the kernel forces delivery at the next user-mode
  // return regardless of alertable state. RtlCloneUserProcess clears the
  // kernel-side APC queue on the cloned thread, so neither pathway has
  // pending work entering libc_fork_reinit.
  //
  // Every call inside the unseal window below — pcb_unseal_readonly_b,
  // pcb_check_canary, ProcessPrng, fix_teb_stack_bounds, the section
  // walker, the per-subsystem reinit thunks, pcb_seal_readonly_b — is
  // non-alertable. The window therefore cannot be reentered by an APC
  // dispatch even if a third-party loader hook later re-queues one.
  // No NtTestAlert / APC-mask call is needed; the closure is structural.
  //
  // Unseal PCB Zone 0b ONLY — Zone 0 (lifetime-immutable PCB constants
  // and the sealed VEH dispatch table) stays sealed across fork. Zone 0b
  // holds the fields that change here: pid, parent_pid, security cookie,
  // dll_notify_cookie. Failure means subsequent Zone 0b writes will AV.
  // =====================================================================
  if (!pcb_unseal_readonly_b())
    ::NtTerminateProcess(NtCurrentProcess(), 127);

  // Validate the inherited canary against the parent's (still-current)
  // cookie BEFORE rotating, so we catch parent-side corruption rather than
  // masking it with a fresh seed.
  if (!pcb_check_canary())
    ::NtTerminateProcess(NtCurrentProcess(), 127);

  // Rotate the PCB-internal cookie so a parent-side info-leak of Zone 0b
  // does not give an attacker the canary value used in the child.
  rotate_pcb_security_cookie();

  // =====================================================================
  // TEB stack bounds — the child's cloned thread resumes on the parent's
  // COW stack, but the kernel created a new TEB with default StackBase/
  // StackLimit/DeallocationStack values that don't match the actual stack.
  // Fix these BEFORE walking .libcfork because the walker may indirectly
  // exercise the VEH handler (which reads gs:0x10 / StackLimit for stack
  // overflow detection).
  // =====================================================================
  windows::fix_teb_stack_bounds();

  // Re-apply the libc stack-overflow handler reserve. Fork delivers the
  // child a fresh TEB with GuaranteedStackBytes = 0 — the parent's value
  // does not survive NtCreateUserProcess (clone mode). Without this the
  // child is one guard-page deep on stack-overflow until pthread_create
  // spawns a new thread that runs apply_libc_stack_guarantee in its own
  // entry. Keep it adjacent to fix_teb_stack_bounds for the same reason
  // — both are TEB-state repairs that the VEH chain depends on.
  windows::apply_libc_stack_guarantee();

  // =====================================================================
  // Walk the .libcfork registry: collect, audit, sort, dispatch.
  // =====================================================================
  auto registry = libc_libcfork_registry();

  // Release-mode hard fail. The .libcfork table is the post-fork dispatch
  // surface; if it is writable an arbitrary-write gadget can repoint any
  // entry's `fn` at attacker code, and the next fork() turns it into a
  // control-flow primitive. Same defensive posture as .libcveh.
  enforce_section_readonly_or_fastfail(g_pcb.zone0.module_handle(),
                                       registry.probe_address(),
                                       ".libcfork");

  // Snapshot the section into a stack array we can sort. Skip padding
  // slots (the $A/$Z bookends and any zero-init alignment padding the
  // linker inserts between $M and $Z): a real LIBC_REGISTER_FORK_REINIT
  // always supplies a non-null fn.
  //
  // Iteration uses `<` not `!=` against end_ because the linker aligns
  // the $Z bookend to alignof(ForkReinitEntry) (8 bytes), not
  // sizeof(ForkReinitEntry) (24 bytes). A range-for using raw-pointer
  // `!=` would step `start+1, start+2, ...` and skip over a misaligned
  // end_, iterating into adjacent section data until it hit a zeroed
  // record. `<` saturates correctly the first time we step past end_.
  ForkReinitEntry sorted[kMaxForkReinitEntries];
  size_t count = 0;
  for (const ForkReinitEntry *p = registry.begin(); p < registry.end(); ++p) {
    if (p->fn == nullptr)
      continue;
    if (count >= kMaxForkReinitEntries) {
      // Build-time invariant breach: more registrants than the bound.
      // Bumping kMaxForkReinitEntries is a one-line change but should be
      // a deliberate decision, not a silent truncation.
      ::NtTerminateProcess(NtCurrentProcess(), 127);
    }
    sorted[count++] = *p;
  }
  sort_by_priority(sorted, count);

  // Dispatch in priority order. Each entry's fn is independently
  // idempotent and runs in the single-threaded fork-child context.
  for (size_t i = 0; i < count; ++i)
    sorted[i].fn();

  // =====================================================================
  // Reseal PCB Zone 0b — canary check + PAGE_READONLY. Zone 0 was never
  // unsealed; the VEH-core reinit (registered in .libcfork at priority
  // kForkPrioVehCore) re-acquires its kernel handle and dll_notify_cookie
  // under the same Zone 0b unseal window opened above.
  // =====================================================================
  if (!pcb_check_canary())
    ::NtTerminateProcess(NtCurrentProcess(), 127);
  // Fail-closed. Reseal failure means the child runs its entire lifetime
  // with Zone 0b PAGE_READWRITE — security_cookie, parent_pid, dll-notify
  // cookie, and the canary triple are all linear-overflow-reachable. The
  // contract Zone 0b exists to enforce ("post-Tier-A, this page is RO
  // except inside narrow unseal windows owned by libc") is violated for
  // the rest of the process, so a downgrade here cannot be silently
  // ridden. NtProtectVirtualMemory R/W → R/O on owned committed pages
  // does not legitimately fail on the supported Windows 11 floor; a
  // failure means an AV/EDR hook is interposing, which is itself a
  // posture we will not run a child process under.
  if (!pcb_seal_readonly_b())
    ::NtTerminateProcess(NtCurrentProcess(), 127);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Force this .obj into static-archive consumers via the fork mini-registry
// glob in fork_reinit.cpp (`LIBC_FORCE_PULL_GLOB("__libc_anchor_fork_*")`).
// libc_fork_reinit_impl IS the strong override of the dispatch entry point;
// the per-subsystem anchors emitted by LIBC_REGISTER_FORK_REINIT are also
// matched by the same glob but are not load-bearing for selection (each
// subsystem TU is reachable via its own DEPENDS chain).
LIBC_ANCHOR_DEFINE(fork, reinit_impl);
