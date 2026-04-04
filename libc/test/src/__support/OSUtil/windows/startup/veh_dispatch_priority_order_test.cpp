//===-- Master VEH dispatch priority-order tests ---------------------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The master VEH handler in `veh/veh_core.cpp` walks the sealed filter table
// in `g_pcb.zone0.veh_sealed().filters[]` from index 0 upward and stops at
// the first filter that returns something other than EXCEPTION_CONTINUE_SEARCH.
// Filters are inserted by `insert_static_veh_filter()` in priority order
// (lower numeric priority = earlier slot index → earlier dispatch).
//
// The invariants under test:
//
//   1. Every filter whose `exception_mask` matches the raised exception's
//      bit is invoked exactly once per fault.
//
//   2. Invocation order is monotonically non-decreasing in `priority`
//      (i.e., lower priority values fire first).
//
//   3. A filter that returns anything other than EXCEPTION_CONTINUE_SEARCH
//      short-circuits dispatch — no later (higher-priority-number) filter
//      runs for that fault.
//
// Strategy is shape (b) from the test brief: the dispatch loop and its
// table both live in sealed Zone 0 memory and the loop itself is a file-
// static function, so a hermetic dispatch-targeted unit test (shape (a))
// would require source modifications. Instead we register four real test-
// owned filters via LIBC_REGISTER_VEH_FILTER at file scope. They land in
// `.libcveh$M`, get swept into the dispatch table during Tier A (Phase
// 0d), and become permanent residents of the sealed table for the test
// process's lifetime — exactly the same code path every production filter
// goes through.
//
// We use exception code EXCEPTION_FLT_INEXACT_RESULT (→ VEH_FLT_INEXACT
// bit) for two reasons:
//
//   - It is a recognised code in `exception_code_to_bit()` (custom codes
//     return 0 and the dispatch loop bails before walking filters).
//
//   - Of the production filters in the table:
//
//       * `mem_fault`        @ VEH_PRIORITY_MEMORY (10)  — claims only
//                                                          VEH_ACCESS_VIOLATION,
//                                                          will not fire.
//       * `mlock_policy`     @ VEH_PRIORITY_MLOCK  (15)  — claims only
//                                                          VEH_GUARD_PAGE,
//                                                          will not fire.
//       * `signal_veh_transport` @ VEH_PRIORITY_SIGNAL (20) — claims
//                                                          VEH_ALL_SIGNAL
//                                                          (includes FPE),
//                                                          BUT returns
//                                                          CONTINUE_SEARCH
//                                                          when no SEH-class
//                                                          signal handler is
//                                                          installed (the
//                                                          test process never
//                                                          installs one).
//
//     So our four test filters at priorities 100/110/120/130 are guaranteed
//     to be the only ones that may produce a non-CONTINUE_SEARCH return for
//     this exception code. Production filters' presence at lower priorities
//     does not affect ordering observations among the test filters.
//
// We dispatch the fault via `RtlRaiseException`, which:
//   - Saves the live CONTEXT with `ExceptionAddress` pointing at the
//     instruction AFTER the RtlRaiseException call.
//   - Routes through the OS exception dispatcher → VEH chain (master
//     handler runs first because veh_core registered with priority 1) →
//     SEH chain.
//   - On EXCEPTION_CONTINUE_EXECUTION, resumes at the saved RIP — i.e.,
//     the test continues normally past the call.
//
// To avoid propagating an unhandled exception to the OS dispatcher when
// every test filter returns CONTINUE_SEARCH, we register one additional
// sink filter at priority 200 (after the four test filters) that matches
// our test exception code and returns CONTINUE_EXECUTION. The sink never
// records in g_recorder, so assertions on fire counts remain unchanged;
// it is invisible unless every earlier filter passed through. Using a
// native VEH filter for the sink keeps this translation unit free of
// `__try`/`__except` and the `__C_specific_handler` compiler-rt personality
// it would otherwise require.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"
#include "src/__support/OSUtil/windows/veh/veh_state.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

extern "C" __LIBC_EXTERN_DLLIMPORT_ATTR void NTAPI
RtlRaiseException(EXCEPTION_RECORD *ExceptionRecord);

namespace {

// ---------------------------------------------------------------------------
// Recorder — shared between the dispatch filters and the test bodies.
//
// `entries[count]` is updated by each filter as it runs. `count` is an
// atomic so that, even though the master dispatcher is synchronous from
// the calling thread's perspective, the C++ memory model is satisfied
// (filters run on the faulting thread, but treating writes as relaxed
// atomics avoids any UB risk if a future change moves dispatch off-thread).
//
// Filter behaviour is keyed off `gate_priority` and `gate_action`:
//
//   - `gate_priority < 0` : every filter is "passive" (records its
//     priority and returns EXCEPTION_CONTINUE_SEARCH so the next one
//     runs).
//
//   - `gate_priority >= 0` : the filter whose own priority equals
//     `gate_priority` returns `gate_action` instead of CONTINUE_SEARCH.
//     All other filters remain passive.
//
// The gate lets us test invariant 3 (short-circuit) without rebuilding
// the dispatch table.
// ---------------------------------------------------------------------------

struct DispatchRecorder {
  static constexpr size_t MAX_ENTRIES = 16;

  LIBC_NAMESPACE::cpp::Atomic<int> count{0};
  uint8_t entries[MAX_ENTRIES] = {};

  // Default: every test filter passes through.
  LIBC_NAMESPACE::cpp::Atomic<int> gate_priority{-1};
  LIBC_NAMESPACE::cpp::Atomic<long> gate_action{EXCEPTION_CONTINUE_SEARCH};

  void reset() {
    count.store(0, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
    for (size_t i = 0; i < MAX_ENTRIES; ++i)
      entries[i] = 0;
    gate_priority.store(-1, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
    gate_action.store(EXCEPTION_CONTINUE_SEARCH,
                      LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  }
};

DispatchRecorder g_recorder;

// Per-priority filter. Templating on the priority constant lets us emit
// four distinct concrete handlers without code duplication while still
// giving each one its own static function symbol that the dispatch table
// can hold.
template <uint8_t Prio>
NTAPI LONG test_filter(EXCEPTION_POINTERS *ep) {
  // Gate every record on the matching exception code. The dispatcher only
  // calls us when `exception_mask & bit` is non-zero, but we want to be
  // bullet-proof against the (unlikely) case that the build accidentally
  // routes some other FPE bit our way — record nothing and pass through.
  if (!ep || !ep->ExceptionRecord ||
      ep->ExceptionRecord->ExceptionCode != EXCEPTION_FLT_INEXACT_RESULT)
    return EXCEPTION_CONTINUE_SEARCH;

  int idx = g_recorder.count.fetch_add(
      1, LIBC_NAMESPACE::cpp::MemoryOrder::ACQ_REL);
  if (idx < static_cast<int>(DispatchRecorder::MAX_ENTRIES))
    g_recorder.entries[idx] = Prio;

  int gate =
      g_recorder.gate_priority.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
  if (gate == static_cast<int>(Prio))
    return static_cast<LONG>(g_recorder.gate_action.load(
        LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE));
  return EXCEPTION_CONTINUE_SEARCH;
}

// Tail-end sink filter at priority 200. Runs only when every earlier
// filter returned CONTINUE_SEARCH (i.e. the passive/non-gated test path)
// and resumes execution past the RtlRaiseException call via
// EXCEPTION_CONTINUE_EXECUTION. Deliberately does NOT update g_recorder
// — it exists purely as the "backstop" and its presence must not perturb
// any count or ordering assertion the test bodies make over the four
// p100..p130 filters.
NTAPI LONG test_sink_filter(EXCEPTION_POINTERS *ep) {
  if (!ep || !ep->ExceptionRecord ||
      ep->ExceptionRecord->ExceptionCode != EXCEPTION_FLT_INEXACT_RESULT)
    return EXCEPTION_CONTINUE_SEARCH;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Direct raise — dispatch is terminated either by a gated test filter or
// by the tail-end sink (priority 200). In neither case does the exception
// leave the VEH chain, so no SEH frame is needed around the call.
[[gnu::noinline]] void raise_flt_inexact() {
  EXCEPTION_RECORD rec = {};
  rec.ExceptionCode = EXCEPTION_FLT_INEXACT_RESULT;
  rec.ExceptionFlags = 0;
  rec.ExceptionRecord = nullptr;
  rec.ExceptionAddress = nullptr;
  rec.NumberParameters = 0;
  RtlRaiseException(&rec);
}

} // namespace

// ---------------------------------------------------------------------------
// Static VEH filters — picked up by the .libcveh sweep during Tier A bring-
// up of the test process. Once Zone 0 is sealed they cannot be removed; that
// is fine for tests because the same process runs every TEST below.
//
// Priorities are deliberately well above every production filter
// (VEH_PRIORITY_SIGNAL == 20) so:
//   - We can reason about ordering among the test filters in isolation.
//   - We do not perturb any production dispatch ordering invariant —
//     production filters are still dispatched first, all return
//     CONTINUE_SEARCH for FPE codes (mem_fault/mlock are mask-mismatched;
//     signal_veh_transport gates on a real signal handler being set), then
//     control reaches our four filters in the order 100 → 110 → 120 → 130.
// ---------------------------------------------------------------------------

LIBC_REGISTER_VEH_FILTER(veh_dispatch_test_p100,
                         ::LIBC_NAMESPACE::windows::VEH_FLT_INEXACT,
                         &test_filter<100>,
                         100)
LIBC_REGISTER_VEH_FILTER(veh_dispatch_test_p110,
                         ::LIBC_NAMESPACE::windows::VEH_FLT_INEXACT,
                         &test_filter<110>,
                         110)
LIBC_REGISTER_VEH_FILTER(veh_dispatch_test_p120,
                         ::LIBC_NAMESPACE::windows::VEH_FLT_INEXACT,
                         &test_filter<120>,
                         120)
LIBC_REGISTER_VEH_FILTER(veh_dispatch_test_p130,
                         ::LIBC_NAMESPACE::windows::VEH_FLT_INEXACT,
                         &test_filter<130>,
                         130)
LIBC_REGISTER_VEH_FILTER(veh_dispatch_test_sink,
                         ::LIBC_NAMESPACE::windows::VEH_FLT_INEXACT,
                         &test_sink_filter,
                         200)

// ---------------------------------------------------------------------------
// Sanity test: prove the four filters actually made it into the sealed
// dispatch table and are stored in priority order. If this fails, every
// later assertion is meaningless — fail loudly here so the failure mode
// is clear ("Tier A sweep didn't place the filters" vs. "dispatch loop
// reorders them").
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehDispatchPriority, TestFiltersAreInstalledInPriorityOrder) {
  const auto &sealed = LIBC_NAMESPACE::g_pcb.zone0.veh_sealed();

  // Walk the sealed table and pick out our test filters by handler
  // identity. We do not assert on absolute slot indices because production
  // filters (mem_fault, mlock_policy, signal_veh_transport) occupy lower
  // slots and their count is implementation-dependent.
  int saw_p100 = -1, saw_p110 = -1, saw_p120 = -1, saw_p130 = -1;
  for (int i = 0; i < sealed.filter_count; ++i) {
    auto *h = sealed.filters[i].handler;
    if (h == &test_filter<100>) saw_p100 = i;
    else if (h == &test_filter<110>) saw_p110 = i;
    else if (h == &test_filter<120>) saw_p120 = i;
    else if (h == &test_filter<130>) saw_p130 = i;
  }

  ASSERT_GE(saw_p100, 0);
  ASSERT_GE(saw_p110, 0);
  ASSERT_GE(saw_p120, 0);
  ASSERT_GE(saw_p130, 0);

  // Lower priority value → earlier slot.
  EXPECT_LT(saw_p100, saw_p110);
  EXPECT_LT(saw_p110, saw_p120);
  EXPECT_LT(saw_p120, saw_p130);

  // Each filter records its own priority, not the slot's.
  EXPECT_EQ(static_cast<int>(sealed.filters[saw_p100].priority), 100);
  EXPECT_EQ(static_cast<int>(sealed.filters[saw_p110].priority), 110);
  EXPECT_EQ(static_cast<int>(sealed.filters[saw_p120].priority), 120);
  EXPECT_EQ(static_cast<int>(sealed.filters[saw_p130].priority), 130);
}

// ---------------------------------------------------------------------------
// Invariants 1 and 2: every matching filter is invoked exactly once and the
// invocation order is monotonically increasing in the priority field.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehDispatchPriority, AllFiltersFireOncePerFaultInPriorityOrder) {
  g_recorder.reset();

  raise_flt_inexact();

  int n = g_recorder.count.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);

  // Every test filter fired exactly once.
  ASSERT_EQ(n, 4);

  // Strict priority order, lower priority value → earlier dispatch.
  EXPECT_EQ(static_cast<int>(g_recorder.entries[0]), 100);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[1]), 110);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[2]), 120);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[3]), 130);
}

// Repeat the same fault and confirm the previous run's bookkeeping is fully
// reset by `reset()` — i.e. there is no per-fault state inside the dispatch
// loop that leaks across invocations.
TEST(LlvmLibcVehDispatchPriority, RepeatedFaultsRedispatchFromScratch) {
  for (int trial = 0; trial < 3; ++trial) {
    g_recorder.reset();
    raise_flt_inexact();
    int n = g_recorder.count.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
    ASSERT_EQ(n, 4) << "trial " << trial << " fired wrong filter count";
    EXPECT_EQ(static_cast<int>(g_recorder.entries[0]), 100);
    EXPECT_EQ(static_cast<int>(g_recorder.entries[1]), 110);
    EXPECT_EQ(static_cast<int>(g_recorder.entries[2]), 120);
    EXPECT_EQ(static_cast<int>(g_recorder.entries[3]), 130);
  }
}

// ---------------------------------------------------------------------------
// Invariant 3: short-circuit. A filter returning a non-CONTINUE_SEARCH value
// must terminate dispatch immediately — no higher-priority-number filter
// runs.
//
// We use EXCEPTION_CONTINUE_EXECUTION (-1) as the gating return because it
// is the only non-CONTINUE_SEARCH value the master dispatcher will faithfully
// hand back to the OS, and the OS will then resume from the saved
// EXCEPTION_POINTERS->ContextRecord — which RtlRaiseException set up to
// point at the instruction *after* the call. So execution returns to the
// `__try` body normally, the `__except` is never entered, and the test
// proceeds.
// ---------------------------------------------------------------------------

TEST(LlvmLibcVehDispatchPriority, NonContinueSearchShortCircuitsDispatch) {
  // Gate the priority-110 filter to claim the exception. Filters at
  // priorities 120 and 130 must not run.
  g_recorder.reset();
  g_recorder.gate_priority.store(110,
                                 LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  g_recorder.gate_action.store(EXCEPTION_CONTINUE_EXECUTION,
                               LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);

  raise_flt_inexact();

  int n = g_recorder.count.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);

  // Exactly two filters ran: 100 (passive) and 110 (claims).
  ASSERT_EQ(n, 2);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[0]), 100);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[1]), 110);

  // Sanity: the higher-priority-number filters are not present in the
  // entry log.
  for (int i = 0; i < n; ++i) {
    EXPECT_NE(static_cast<int>(g_recorder.entries[i]), 120);
    EXPECT_NE(static_cast<int>(g_recorder.entries[i]), 130);
  }
}

// Gate the very first test filter (priority 100). Only that one filter
// should run; the dispatcher must not visit 110/120/130.
TEST(LlvmLibcVehDispatchPriority, FirstTestFilterClaimingStopsAfterOne) {
  g_recorder.reset();
  g_recorder.gate_priority.store(100,
                                 LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  g_recorder.gate_action.store(EXCEPTION_CONTINUE_EXECUTION,
                               LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);

  raise_flt_inexact();

  int n = g_recorder.count.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[0]), 100);
}

// Gate the last test filter (priority 130). All four must run, in order;
// the only observable effect of the gate is the absence of SEH `__except`
// activation — which we cannot directly assert here without further
// instrumentation, but the count and order assertions still constrain the
// dispatcher's behaviour (it must walk every priority slot before stopping).
TEST(LlvmLibcVehDispatchPriority, LastTestFilterClaimingStillRunsAllFour) {
  g_recorder.reset();
  g_recorder.gate_priority.store(130,
                                 LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  g_recorder.gate_action.store(EXCEPTION_CONTINUE_EXECUTION,
                               LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);

  raise_flt_inexact();

  int n = g_recorder.count.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
  ASSERT_EQ(n, 4);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[0]), 100);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[1]), 110);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[2]), 120);
  EXPECT_EQ(static_cast<int>(g_recorder.entries[3]), 130);
}
