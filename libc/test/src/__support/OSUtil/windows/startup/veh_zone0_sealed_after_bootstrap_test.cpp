//===-- VEH dispatch state Zone 0 seal AV death tests -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The unified VEH dispatch table — `VehSealedState` — lives inside Zone 0
// of the PCB (see process_control_block.h, veh_state.h) and is sealed
// PAGE_READONLY at the end of Tier A. There is intentionally NO runtime
// `register_veh_filter` API; every filter is statically registered through
// the `.libcveh` COFF section and the table is swept once during Phase 0d
// of `__libc_bootstrap()` before the seal closes (see veh_filter_registry.h
// and veh_core.cpp). The sealed table is then read by `master_veh_handler`
// without synchronisation — an attacker with arbitrary write must NOT be
// able to redirect dispatch.
//
// This test is the runtime witness for that contract:
//
//   1. After bootstrap returns, the VEH dispatch state is non-zero — the
//      master handler has been registered, the `.libcveh` sweep populated
//      filters[], and the fault-guard / reentry-guard TLS indices have
//      been allocated. (If any of these are zero, either bootstrap did
//      not run, or a refactor has broken Tier A phase ordering.)
//
//   2. Writes to ANY field inside `VehSealedState` raise
//      EXCEPTION_ACCESS_VIOLATION. We probe `filter_count`, the first
//      `VehFilter` slot's handler pointer, and both TLS indices — covering
//      the three substructures an attacker would target to redirect
//      dispatch (count for table-walk truncation, handler for code
//      redirection, TLS index for guard bypass).
//
//   3. Two consecutive reads of the sealed snapshot return identical
//      `filter_count` and the same handler pointers. Mutation of these
//      fields after Tier A is impossible by construction (sealed page),
//      so this asserts the read API surfaces stable state — a regression
//      where some code path silently swapped the table at runtime would
//      surface here even before the AV probe.
//
// Pattern mirrors slab_pool_death_test.cpp and the sibling
// pcb_zone0_seal_av_test.cpp — fork; the child performs the illegal write
// through a `volatile` pointer obtained by `const_cast`'ing away const-ness
// of the sealed accessor; the parent waitpids and asserts WIFSIGNALED with
// WTERMSIG == SIGSEGV. If the write completes and the child reports
// REACHED_END, the seal regressed.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/type_traits/is_same.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/veh/veh_state.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/signal-macros.h"
#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

namespace {

// Sentinels so a regression (write completed without a fault) surfaces as
// a distinct exit code rather than as a plain zero.
enum : int {
  REACHED_END = 77,
};

// Helper: get a writable pointer to the sealed VehSealedState by stripping
// the const off the public accessor's return reference. The compiler
// cannot prove the subsequent store is dead, so it is emitted; on a
// correctly sealed page the store raises EXCEPTION_ACCESS_VIOLATION.
[[gnu::noinline]] LIBC_NAMESPACE::windows::VehSealedState *sealed_writable() {
  const auto &ro = LIBC_NAMESPACE::g_pcb.zone0.veh_sealed();
  return const_cast<LIBC_NAMESPACE::windows::VehSealedState *>(&ro);
}

// Probes — each writes ONE field of VehSealedState. We split them so a
// regression that only de-seals one substructure (e.g. an unintended
// VirtualProtect of just the filter array) surfaces independently. Each
// is its own [[gnu::noinline]] function to keep the offending instruction
// pinned at a known callsite for debugger triage.

[[gnu::noinline]] void poke_veh_filter_count() {
  auto *s = sealed_writable();
  auto *probe = reinterpret_cast<volatile int *>(&s->filter_count);
  *probe = 0; // would zero-out the dispatch table walk → must AV
}

[[gnu::noinline]] void poke_veh_filter0_handler() {
  auto *s = sealed_writable();
  // filters[0].handler is the first VehFilter's callback pointer. Writing
  // it would redirect priority-0 exception dispatch to attacker code; the
  // seal must reject the write.
  auto *probe =
      reinterpret_cast<volatile uintptr_t *>(&s->filters[0].handler);
  *probe = 0xDEADBEEFCAFEBABEULL;
}

[[gnu::noinline]] void poke_veh_reentry_tls_index() {
  auto *s = sealed_writable();
  auto *probe = reinterpret_cast<volatile unsigned *>(&s->reentry_tls_index);
  *probe = 0xFFFFFFFFu; // bogus TLS slot would silently bypass reentry guard
}

[[gnu::noinline]] void poke_veh_fault_guard_tls_index() {
  auto *s = sealed_writable();
  auto *probe =
      reinterpret_cast<volatile unsigned *>(&s->fault_guard_tls_index);
  *probe = 0xFFFFFFFFu;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. After bootstrap, the VEH dispatch state must be populated.
//
// Sanity preconditions for the death tests below: if filter_count is zero
// or the TLS indices are zero, the sealed page might be RO-but-empty and
// the AV checks below would still pass for the wrong reason.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehZone0Sealed, DispatchStateIsLiveAfterBootstrap) {
  const auto &sealed = LIBC_NAMESPACE::g_pcb.zone0.veh_sealed();

  // Tier A must have run the `.libcveh` sweep — at least one filter
  // (the memory subsystem's AV guard at priority 10) is statically
  // registered, so filter_count is strictly > 0.
  EXPECT_GT(sealed.filter_count, 0);
  EXPECT_LE(sealed.filter_count, LIBC_NAMESPACE::windows::VEH_MAX_FILTERS);

  // Every populated filter slot must have a non-null handler. A zero
  // handler would be a `.libcveh` sweep bug or a layout mismatch.
  for (int i = 0; i < sealed.filter_count; ++i) {
    EXPECT_NE(sealed.filters[i].handler,
              static_cast<LONG (*)(EXCEPTION_POINTERS *)>(nullptr));
    EXPECT_NE(sealed.filters[i].exception_mask, 0u);
  }

  // Filters must be sorted by ascending priority (insertion-sort invariant
  // of insert_static_veh_filter). This catches a regression where the
  // sweep order or comparator changes silently.
  for (int i = 1; i < sealed.filter_count; ++i) {
    EXPECT_LE(sealed.filters[i - 1].priority, sealed.filters[i].priority);
  }

  // Both TLS indices must have been allocated — Phase 0a allocates the
  // reentry guard slot, Phase 0b the fault guard slot. PEB TLS indices
  // are 1-based for our purposes (slot 0 is reserved by the loader);
  // however we only require non-zero here as a coarse "was it set"
  // signal.
  EXPECT_NE(sealed.reentry_tls_index, 0u);
  EXPECT_NE(sealed.fault_guard_tls_index, 0u);
}

// ---------------------------------------------------------------------------
// 2. Two consecutive reads of the sealed snapshot must agree.
//
// Sealed memory cannot mutate, so this is a tautology IF the seal is in
// place. The point is to catch a regression where some other code path
// (e.g. a misplaced unseal/reseal pair) briefly swaps the table while a
// reader is mid-walk. Done in-process — no fork.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehZone0Sealed, RepeatedReadsAreStable) {
  const auto &s1 = LIBC_NAMESPACE::g_pcb.zone0.veh_sealed();
  int count1 = s1.filter_count;
  void *handlers1[LIBC_NAMESPACE::windows::VEH_MAX_FILTERS] = {};
  uint32_t masks1[LIBC_NAMESPACE::windows::VEH_MAX_FILTERS] = {};
  for (int i = 0; i < count1; ++i) {
    handlers1[i] = reinterpret_cast<void *>(s1.filters[i].handler);
    masks1[i] = s1.filters[i].exception_mask;
  }

  // Spin briefly — anything racing to mutate the table would have a
  // window to land its store. (In practice, no such code exists; this
  // simply demonstrates the immutability claim has actual teeth.)
  for (volatile int i = 0; i < 1024; ++i) {
    (void)i;
  }

  const auto &s2 = LIBC_NAMESPACE::g_pcb.zone0.veh_sealed();
  EXPECT_EQ(s2.filter_count, count1);
  for (int i = 0; i < count1; ++i) {
    EXPECT_EQ(reinterpret_cast<void *>(s2.filters[i].handler), handlers1[i]);
    EXPECT_EQ(s2.filters[i].exception_mask, masks1[i]);
  }

  // TLS indices likewise stable.
  EXPECT_EQ(s2.reentry_tls_index, s1.reentry_tls_index);
  EXPECT_EQ(s2.fault_guard_tls_index, s1.fault_guard_tls_index);
}

// ---------------------------------------------------------------------------
// 3a. Writing filter_count must AV.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehZone0SealedDeath, WriteFilterCountFaultsWithSigsegv) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Force the read to be observable so nothing optimises the probe away.
    (void)LIBC_NAMESPACE::g_pcb.zone0.veh_sealed().filter_count;
    poke_veh_filter_count();
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}

// ---------------------------------------------------------------------------
// 3b. Writing the first filter slot's handler pointer must AV.
//
// This is the most security-critical field — if it were writable, an
// arbitrary-write primitive could redirect priority-0 exception dispatch
// to attacker code. The seal is the entire point.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehZone0SealedDeath, WriteFilterHandlerFaultsWithSigsegv) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    (void)LIBC_NAMESPACE::g_pcb.zone0.veh_sealed().filters[0].handler;
    poke_veh_filter0_handler();
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}

// ---------------------------------------------------------------------------
// 3c. Writing reentry_tls_index must AV. A bogus index would silently
//     skip the reentry guard, allowing infinite VEH recursion.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehZone0SealedDeath, WriteReentryTlsIndexFaultsWithSigsegv) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    (void)LIBC_NAMESPACE::g_pcb.zone0.veh_sealed().reentry_tls_index;
    poke_veh_reentry_tls_index();
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}

// ---------------------------------------------------------------------------
// 3d. Writing fault_guard_tls_index must AV. Same threat model as
//     reentry_tls_index — a redirected slot lets a faulting libc page
//     probe escape into longjmp on a chain head the attacker controls.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehZone0SealedDeath, WriteFaultGuardTlsIndexFaultsWithSigsegv) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    (void)LIBC_NAMESPACE::g_pcb.zone0.veh_sealed().fault_guard_tls_index;
    poke_veh_fault_guard_tls_index();
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}

// ---------------------------------------------------------------------------
// 4. There is NO runtime `register_veh_filter` API.
//
// The .libcveh COFF section is the SOLE install mechanism (see
// veh_filter_registry.h header comment: "Replaces the former dynamic
// register_veh_filter / unregister_veh_filter API"). If someone reintroduces
// such a symbol — even an internal one — this is a regression of the
// sealed-table contract: a runtime mutator implies the table cannot stay
// in PAGE_READONLY memory. We cannot grep the source tree at test time,
// so the closest runtime witness is: the sealed snapshot returned by the
// public accessor must be a `const`-qualified reference, and the only
// documented mutation entry point (insert_static_veh_filter) is invoked
// solely from `register_all_static_veh_filters()` during Tier A. After
// Tier A the seal is in place and the AV death tests above are the
// runtime guarantee that no reachable mutator exists.
//
// This test compiles a static_assert pinning the accessor return type;
// any change that exposes a non-const overload (or relaxes the seal)
// trips the build before runtime.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehZone0Sealed, AccessorReturnsConstReference) {
  using AccessorRef =
      decltype(LIBC_NAMESPACE::g_pcb.zone0.veh_sealed());
  static_assert(
      LIBC_NAMESPACE::cpp::is_same_v<
          AccessorRef, const LIBC_NAMESPACE::windows::VehSealedState &>,
      "veh_sealed() must return a const reference — a non-const overload "
      "would expose a runtime mutation path that the Zone 0 seal forbids");
  // Runtime body is a tautology — the static_assert is the real check.
  EXPECT_TRUE(true);
}
