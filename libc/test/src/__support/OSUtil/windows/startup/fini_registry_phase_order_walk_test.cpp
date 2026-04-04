//===-- .libcfin phased registry reverse-walk order test -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `run_all_finis()` walks `.libcfin$P9..$P0` in reverse phase order; the
// production walker is the loop in libc_fini_registry.cpp. The contract
// the rest of the libc relies on is:
//
//   - Every record registered in any `$P0..$P9` bucket is visited exactly
//     once per call.
//   - Visitation is in strict descending phase order: every record in
//     `$P9` walks before every record in `$P8`, ..., before every record
//     in `$P0`.
//   - Within a single phase bucket the linker merge order is undefined
//     (and explicitly documented in libc_fini_registry.h as such), so we
//     assert no intra-bucket order.
//   - Empty phase buckets produce zero records and do NOT collapse
//     adjacent buckets together — i.e. the walker traverses an empty
//     phase as a no-op and continues into the next non-empty one.
//
// We cannot reuse the real `.libcfin` section here: every libc subsystem
// has live records in it (alpc_bus, signal, reactor, fd_table, ...) and
// invoking those mid-test would tear down the test process. Strategy (a)
// from the section_registry.h docs applies: we declare a parallel
// PRIVATE phased registry under a different section base name
// (`.libcfintest`), register test-local thunks into it, and walk it via
// the same `SectionRegistry<Record>::reverse()` iteration template the
// production walker uses. That exercises exactly the reverse-walk
// machinery the production code depends on, without touching live finis.
//
// We deliberately test four NON-CONSECUTIVE phases (9, 6, 3, 0) so that
// (i) the empty-phase invariant is exercised on the gaps between them
// and (ii) any accidental `$P9 → $P8 → $P7 → ...` sequential bug would
// still be caught (the expected sequence skips P8/P7/P5/P4/P2/P1).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/macros/config.h"
#include "test/UnitTest/Test.h"

// ---------------------------------------------------------------------------
// Private parallel registry — `.libcfintest$A`, `$P0..$P9`, `$Z`.
// Lives entirely in this TU; nobody else registers into it.
// ---------------------------------------------------------------------------

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// One entry per registered test fini. The thunk closes over a phase id
// the test can observe in the recorded order.
struct FiniOrderEntry {
  void (*thunk)();
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_DEFINE_SECTION_BOOKENDS(libcfintest,
                             ::LIBC_NAMESPACE::internal::FiniOrderEntry)

// ---------------------------------------------------------------------------
// Recorder + per-phase thunks. The recorder is a plain file-scope array
// so the thunks (which must be at namespace scope to satisfy the
// LIBC_REGISTER_FINI / LIBC_SECTION_REGISTER_PHASED macro contract) can
// reach it without TLS or a singleton.
//
// `record_phase()` is the single mutator. We reset `g_count = 0` at
// the start of every test, then walk and let each thunk push its phase
// number. Capacity 16 is comfortably above the four phases we register.
// ---------------------------------------------------------------------------

namespace fini_phase_order_test {

static constexpr unsigned kCapacity = 16;
static int g_recorded[kCapacity];
static unsigned g_count = 0;

static void record_phase(int phase) {
  if (g_count < kCapacity)
    g_recorded[g_count++] = phase;
}

static void thunk_p9() { record_phase(9); }
static void thunk_p6() { record_phase(6); }
static void thunk_p3() { record_phase(3); }
static void thunk_p0() { record_phase(0); }

} // namespace fini_phase_order_test

// LIBC_SECTION_REGISTER_PHASED expands at namespace scope. Tags must be
// unique across the link; the `fini_phase_order_test_*` prefix avoids
// any collision with real subsystem tags in `.libcfin`.
LIBC_SECTION_REGISTER_PHASED(libcfintest,
                             ::LIBC_NAMESPACE::internal::FiniOrderEntry, 9,
                             fini_phase_order_test_p9,
                             {&::fini_phase_order_test::thunk_p9})
LIBC_SECTION_REGISTER_PHASED(libcfintest,
                             ::LIBC_NAMESPACE::internal::FiniOrderEntry, 6,
                             fini_phase_order_test_p6,
                             {&::fini_phase_order_test::thunk_p6})
LIBC_SECTION_REGISTER_PHASED(libcfintest,
                             ::LIBC_NAMESPACE::internal::FiniOrderEntry, 3,
                             fini_phase_order_test_p3,
                             {&::fini_phase_order_test::thunk_p3})
LIBC_SECTION_REGISTER_PHASED(libcfintest,
                             ::LIBC_NAMESPACE::internal::FiniOrderEntry, 0,
                             fini_phase_order_test_p0,
                             {&::fini_phase_order_test::thunk_p0})

namespace {

// Mirrors the loop body in libc_fini_registry.cpp::run_all_finis(),
// minus the FaultGuard wrapping (which depends on live VEH state we do
// not want to perturb during a unit test). The reverse-walk iteration
// itself is the property under test.
void walk_test_finis() {
  auto registry = ::LIBC_NAMESPACE::internal::libc_libcfintest_registry();
  for (const auto &entry : registry.reverse())
    entry.thunk();
}

void reset_recorder() {
  for (unsigned i = 0; i < fini_phase_order_test::kCapacity; ++i)
    fini_phase_order_test::g_recorded[i] = -1;
  fini_phase_order_test::g_count = 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Walk yields exactly the four registered phases in strict descending
// order. This single test covers the four invariants documented at the
// top of this file:
//
//   - Visited exactly once  →  count == 4.
//   - Strict $P9 → $P0      →  recorded[i] > recorded[i+1] for all i.
//   - $PN never before $PM (M > N)  →  follows from strict descending.
//   - Empty phases handled  →  recorded sequence is exactly {9,6,3,0},
//                              never widened to include phantom entries
//                              from $P8/$P7/$P5/$P4/$P2/$P1.
// ---------------------------------------------------------------------------
TEST(LlvmLibcFiniRegistryPhaseOrder, ReverseWalkVisitsP9ToP0Strict) {
  reset_recorder();

  walk_test_finis();

  ASSERT_EQ(fini_phase_order_test::g_count, 4u);

  // Exact expected sequence — descending, gaps preserved.
  EXPECT_EQ(fini_phase_order_test::g_recorded[0], 9);
  EXPECT_EQ(fini_phase_order_test::g_recorded[1], 6);
  EXPECT_EQ(fini_phase_order_test::g_recorded[2], 3);
  EXPECT_EQ(fini_phase_order_test::g_recorded[3], 0);

  // Strict-descending invariant restated as a generic check so a future
  // test author who adds another phase only has to extend the registers
  // above; this loop keeps catching out-of-order bugs without an edit.
  for (unsigned i = 1; i < fini_phase_order_test::g_count; ++i) {
    EXPECT_GT(fini_phase_order_test::g_recorded[i - 1],
              fini_phase_order_test::g_recorded[i]);
  }
}

// ---------------------------------------------------------------------------
// A second walk over the same registry reproduces the same sequence.
// `run_all_finis()` is single-shot in production (the loader unmaps the
// image right after), but the SectionRegistry<>::reverse() iterator is
// the same template used by other registries that ARE walked
// repeatedly (e.g. lazy_init_reset). Idempotency of the walker is part
// of the registry primitive's contract.
// ---------------------------------------------------------------------------
TEST(LlvmLibcFiniRegistryPhaseOrder, ReverseWalkIsIdempotent) {
  reset_recorder();
  walk_test_finis();
  ASSERT_EQ(fini_phase_order_test::g_count, 4u);
  int first[4] = {fini_phase_order_test::g_recorded[0],
                  fini_phase_order_test::g_recorded[1],
                  fini_phase_order_test::g_recorded[2],
                  fini_phase_order_test::g_recorded[3]};

  reset_recorder();
  walk_test_finis();
  ASSERT_EQ(fini_phase_order_test::g_count, 4u);

  for (unsigned i = 0; i < 4; ++i)
    EXPECT_EQ(first[i], fini_phase_order_test::g_recorded[i]);
}
