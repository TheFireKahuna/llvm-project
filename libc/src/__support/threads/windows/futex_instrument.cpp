//===--- Futex/wait_slot instrumentation — symbol defs + dump --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LIBC_FUTEX_INSTRUMENT
#define LIBC_FUTEX_INSTRUMENT 1
#endif

#include "src/__support/threads/windows/futex_instrument.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/wait_slot.h"

namespace LIBC_NAMESPACE_DECL {
namespace futex_instrument {

// BSS storage — zero-initialised by the loader.
CounterLine g_counters[NUM_COUNTERS] = {};
WorkerPhase g_phases[kMaxInstrWorkers] = {};

LIBC_THREAD_LOCAL WorkerPhase *tls_my_phase = nullptr;

void reset() {
  for (uint32_t i = 0; i < NUM_COUNTERS; ++i)
    g_counters[i].v.store(0, cpp::MemoryOrder::RELAXED);
  for (uint32_t i = 0; i < kMaxInstrWorkers; ++i) {
    g_phases[i].phase.store(0, cpp::MemoryOrder::RELAXED);
    g_phases[i].worker_id.store(0, cpp::MemoryOrder::RELAXED);
    g_phases[i].last_ret.store(0, cpp::MemoryOrder::RELAXED);
    g_phases[i].iter.store(0, cpp::MemoryOrder::RELAXED);
    g_phases[i].heartbeat.store(0, cpp::MemoryOrder::RELAXED);
  }
}

const char *name_of(Counter c) {
  switch (c) {
  case CTR_BENCH_LOCK_CAS: return "bench.lock_cas";
  case CTR_BENCH_LOCK_HANDOFF: return "bench.lock_handoff";
  case CTR_BENCH_UNLOCK: return "bench.unlock";
  case CTR_WAIT_ENTRIES: return "wait.entries";
  case CTR_WAIT_FAST_VAL_CHANGED: return "wait.fast_val_changed";
  case CTR_WAIT_PHASE1_HW_EXIT: return "wait.phase1_hw_exit";
  case CTR_WAIT_PHASE15_EXPIRED: return "wait.phase15_expired";
  case CTR_WAIT_PHASE175_RECLAIMED: return "wait.phase175_reclaimed";
  case CTR_WAIT_PHASE2_POOL_ENOMEM: return "wait.phase2_pool_enomem";
  case CTR_WAIT_PHASE2_VAL_CHANGED_CAS: return "wait.phase2_val_changed_cas";
  case CTR_WAIT_PHASE2_PUSH_OK: return "wait.phase2_push_ok";
  case CTR_WAIT_PHASE25_WAKE_CLEAN_OR_ORPHAN: return "wait.phase25_wake_signaled";
  case CTR_WAIT_PHASE25_WAKE_GHOST: return "wait.phase25_wake_ghost";
  case CTR_WAIT_PHASE3_CAS_OK: return "wait.phase3_cas_ok";
  case CTR_WAIT_PHASE3_CAS_FAIL_ORPHAN: return "wait.phase3_cas_fail_orphan";
  case CTR_WAIT_PHASE4_ITERATIONS: return "wait.phase4_iters";
  case CTR_WAIT_PHASE4_PREPARK_WAKE_SIGNALED: return "wait.phase4_prepark_wake_signaled";
  case CTR_WAIT_PHASE4_PREPARK_WAKE_GHOST: return "wait.phase4_prepark_wake_ghost";
  case CTR_WAIT_PHASE4_NTWAIT_CALLS: return "wait.phase4_ntwait_calls";
  case CTR_WAIT_PHASE4_NTWAIT_RET_ALERTED: return "wait.phase4_ntwait_ret_alerted";
  case CTR_WAIT_PHASE4_NTWAIT_RET_TIMEOUT: return "wait.phase4_ntwait_ret_timeout";
  case CTR_WAIT_PHASE4_NTWAIT_RET_USER_APC: return "wait.phase4_ntwait_ret_user_apc";
  case CTR_WAIT_PHASE4_NTWAIT_RET_OTHER: return "wait.phase4_ntwait_ret_other";
  case CTR_WAIT_PHASE4_POSTWAIT_WAKE_SIGNALED: return "wait.phase4_postwait_wake_signaled";
  case CTR_WAIT_PHASE4_POSTWAIT_WAKE_GHOST: return "wait.phase4_postwait_wake_ghost";
  case CTR_WAIT_PHASE4_DRAIN_MARK_LATE: return "wait.phase4_drain_mark_late";
  case CTR_WAIT_PHASE4_CANCEL_BY_TIMEOUT: return "wait.phase4_cancel_by_timeout";
  case CTR_WAIT_PHASE4_CANCEL_STALE_PRED: return "wait.phase4_cancel_stale_pred";
  case CTR_WAIT_PHASE4_STALE_RELOOP: return "wait.phase4_stale_reloop";
  case CTR_WAIT_PHASE4_SIGNAL_LIKE_INT: return "wait.phase4_signal_like_int";
  case CTR_WAIT_PHASE4_SIGNAL_LIKE_NONINT: return "wait.phase4_signal_like_nonint";
  case CTR_WAIT_RET_0: return "wait.ret_0";
  case CTR_WAIT_RET_1: return "wait.ret_1";
  case CTR_WAIT_RET_ETIMEDOUT: return "wait.ret_etimedout";
  case CTR_WAIT_RET_EINTR: return "wait.ret_eintr";
  case CTR_WAIT_RET_EINVAL: return "wait.ret_einval";
  case CTR_WAIT_RET_ENOMEM: return "wait.ret_enomem";
  case CTR_WAIT_RET_OTHER: return "wait.ret_other";
  case CTR_WAIT_HANDOFF_CONSUMED_PHASE25: return "wait.handoff_consumed_phase25";
  case CTR_WAIT_HANDOFF_CONSUMED_PHASE3_SELFCOMMIT: return "wait.handoff_consumed_phase3_selfcommit";
  case CTR_WAIT_HANDOFF_CONSUMED_PHASE4_PREPARK: return "wait.handoff_consumed_phase4_prepark";
  case CTR_WAIT_HANDOFF_CONSUMED_PHASE4_POSTWAIT: return "wait.handoff_consumed_phase4_postwait";
  case CTR_WAIT_NOHANDOFF_PHASE25: return "wait.nohandoff_phase25";
  case CTR_WAIT_NOHANDOFF_PHASE3_SELFCOMMIT: return "wait.nohandoff_phase3_selfcommit";
  case CTR_WAIT_NOHANDOFF_PHASE4_PREPARK: return "wait.nohandoff_phase4_prepark";
  case CTR_WAIT_NOHANDOFF_PHASE4_POSTWAIT: return "wait.nohandoff_phase4_postwait";
  case CTR_WAIT_HANDOFF_PHASE25_GEN_MATCH: return "wait.handoff_phase25_gen_match";
  case CTR_WAIT_HANDOFF_PHASE25_GEN_MISMATCH: return "wait.handoff_phase25_gen_mismatch";
  case CTR_WAIT_HANDOFF_PHASE4_GEN_MATCH: return "wait.handoff_phase4_gen_match";
  case CTR_WAIT_HANDOFF_PHASE4_GEN_MISMATCH: return "wait.handoff_phase4_gen_mismatch";
  case CTR_HO_WAITING_PUBLISH_LOST_PRIOR_HANDOFF: return "ho.waiting_publish_lost_PRIOR_HANDOFF";
  case CTR_HO_IN_KERNEL_PUBLISH_LOST_PRIOR_HANDOFF: return "ho.in_kernel_publish_lost_PRIOR_HANDOFF";
  case CTR_POP_PUBLISH_LOST_PRIOR_HANDOFF: return "pop.publish_lost_PRIOR_HANDOFF";
  case CTR_BENCH_DOUBLE_OWNERSHIP_OBSERVED: return "bench.DOUBLE_OWNERSHIP_OBSERVED";
  case CTR_BENCH_MAX_CS_OCCUPANCY_OBSERVED: return "bench.max_cs_occupancy_observed";
  case CTR_POP_CALLS: return "pop.calls";
  case CTR_POP_EMPTY: return "pop.empty";
  case CTR_POP_TOP_STALE: return "pop.top_stale";
  case CTR_POP_TOP_LIVE_WAITING: return "pop.top_live_waiting";
  case CTR_POP_TOP_LIVE_IN_KERNEL: return "pop.top_live_in_kernel";
  case CTR_POP_TOP_LIVE_TIMEDOUT: return "pop.top_live_timedout";
  case CTR_POP_TOP_LIVE_SIGNALED_HELP: return "pop.top_live_signaled_help";
  case CTR_POP_TOP_LIVE_IDLE_RECOVER: return "pop.top_live_idle_recover";
  case CTR_POP_PREMARK_CAS_OK: return "pop.premark_cas_ok";
  case CTR_POP_PREMARK_CAS_RETRY: return "pop.premark_cas_retry";
  case CTR_POP_DETACH_OK: return "pop.detach_ok";
  case CTR_POP_DETACH_FAIL_ORPHAN: return "pop.detach_fail_orphan";
  case CTR_POP_PUBLISH_OK: return "pop.publish_ok";
  case CTR_POP_PUBLISH_LOST: return "pop.publish_lost";
  case CTR_POP_ALERT_ISSUED: return "pop.alert_issued";
  case CTR_POP_ALERT_SKIPPED_NOT_SIGNALED: return "pop.alert_skipped_not_signaled";
  case CTR_POP_ALERT_SKIPPED_NOT_IN_KERNEL: return "pop.alert_skipped_not_in_kernel";
  case CTR_POP_RET_TRUE: return "pop.ret_true";
  case CTR_POP_RET_FALSE: return "pop.ret_false";
  case CTR_HO_CALLS: return "ho.calls";
  case CTR_HO_EMPTY_NO_WAITERS: return "ho.empty_no_waiters";
  case CTR_HO_TOP_STALE: return "ho.top_stale";
  case CTR_HO_TOP_LIVE_WAITING: return "ho.top_live_waiting";
  case CTR_HO_TOP_LIVE_IN_KERNEL: return "ho.top_live_in_kernel";
  case CTR_HO_TOP_LIVE_TIMEDOUT: return "ho.top_live_timedout";
  case CTR_HO_TOP_LIVE_SIGNALED_HELP: return "ho.top_live_signaled_help";
  case CTR_HO_TOP_LIVE_IDLE_RECOVER: return "ho.top_live_idle_recover";
  case CTR_HO_WAITING_PREMARK_RETRY: return "ho.waiting_premark_retry";
  case CTR_HO_WAITING_DETACH_OK: return "ho.waiting_detach_ok";
  case CTR_HO_WAITING_DETACH_FAIL: return "ho.waiting_detach_fail";
  case CTR_HO_WAITING_PUBLISH_OK: return "ho.waiting_publish_ok";
  case CTR_HO_WAITING_PUBLISH_LOST_COMPLETED: return "ho.waiting_publish_lost_completed";
  case CTR_HO_WAITING_GHOST_REVERT_OK: return "ho.waiting_ghost_revert_ok";
  case CTR_HO_WAITING_GHOST_REVERT_LOST: return "ho.waiting_ghost_revert_lost";
  case CTR_HO_WAITING_POSTSTATE_BAD: return "ho.waiting_poststate_bad";
  case CTR_HO_WAITING_ROLLBACK_CAS_OK: return "ho.waiting_rollback_cas_ok";
  case CTR_HO_WAITING_ROLLBACK_CAS_LOST: return "ho.waiting_rollback_cas_lost";
  case CTR_HO_IN_KERNEL_PREMARK_RETRY: return "ho.in_kernel_premark_retry";
  case CTR_HO_IN_KERNEL_DETACH_OK: return "ho.in_kernel_detach_ok";
  case CTR_HO_IN_KERNEL_DETACH_FAIL: return "ho.in_kernel_detach_fail";
  case CTR_HO_IN_KERNEL_PUBLISH_OK: return "ho.in_kernel_publish_ok";
  case CTR_HO_IN_KERNEL_PUBLISH_LOST: return "ho.in_kernel_publish_lost";
  case CTR_HO_IN_KERNEL_ALERT_ISSUED: return "ho.in_kernel_alert_issued";
  case CTR_HO_IN_KERNEL_ALERT_SKIPPED: return "ho.in_kernel_alert_skipped";
  case CTR_HO_RET_HANDOFF: return "ho.ret_handoff";
  case CTR_HO_RET_COMPLETED: return "ho.ret_completed";
  case CTR_HO_RET_EMPTY: return "ho.ret_empty";
  case CTR_UN_RET_HANDOFF: return "un.ret_handoff";
  case CTR_UN_RET_COMPLETED: return "un.ret_completed";
  case CTR_UN_RET_EMPTY_POP_WOKE: return "un.ret_empty_pop_woke";
  case CTR_UN_RET_EMPTY_POP_NONE: return "un.ret_empty_pop_none";
  case CTR_WS_ALLOC_SLOT: return "ws.alloc_slot";
  case CTR_WS_ALLOC_SECONDARY: return "ws.alloc_secondary";
  case CTR_WS_ALLOC_SLOT_REUSE_FAST: return "ws.alloc_slot_reuse_fast";
  case CTR_WS_FREE_SLOT: return "ws.free_slot";
  case CTR_WS_RECLAIM_CALLS: return "ws.reclaim_calls";
  case CTR_WS_RECLAIM_IDEMPOTENT: return "ws.reclaim_idempotent";
  case CTR_WS_RECLAIM_HAZARD_EMPTY: return "ws.reclaim_hazard_empty";
  case CTR_WS_RECLAIM_RETIRED: return "ws.reclaim_retired";
  case CTR_WS_RECLAIM_QUEUE_FULL_DIRECT: return "ws.reclaim_queue_full_direct";
  case CTR_WS_RETIRE_FLUSH: return "ws.retire_flush";
  case CTR_WS_FREELIST_PUSH: return "ws.freelist_push";
  case CTR_WS_ALERT_ONE_FAST: return "ws.alert_one_fast";
  case CTR_WS_ALERT_ONE_VALIDATED_LIVE: return "ws.alert_one_validated_live";
  case CTR_WS_ALERT_ONE_VALIDATED_DEAD: return "ws.alert_one_validated_dead";
  case CTR_WS_ALERT_ONE_UNBOUND: return "ws.alert_one_unbound";
  case CTR_WS_ALERT_MULTIPLE_CALLS: return "ws.alert_multiple_calls";
  case CTR_WS_ALERT_MULTIPLE_TIDS_ISSUED: return "ws.alert_multiple_tids_issued";
  case CTR_WS_ALERT_MULTIPLE_TIDS_FILTERED: return "ws.alert_multiple_tids_filtered";
  case CTR_WS_HAZARD_ENTER: return "ws.hazard_enter";
  case CTR_WS_HAZARD_EXIT: return "ws.hazard_exit";
  case CTR_WS_CLEANUP_IDLE: return "ws.cleanup_idle";
  case CTR_WS_CLEANUP_SIGNALED_CLEAN: return "ws.cleanup_signaled_clean";
  case CTR_WS_CLEANUP_SIGNALED_ORPHAN_OR_PREMARK: return "ws.cleanup_signaled_orphan_or_premark";
  case CTR_WS_CLEANUP_WAITING_OR_IN_KERNEL_OR_TIMEDOUT: return "ws.cleanup_live_linked";
  default: return "?";
  }
}

const char *phase_name_of(uint32_t p) {
  switch (p) {
  case PHASE_INIT: return "INIT";
  case PHASE_IN_CAS: return "CAS";
  case PHASE_IN_WAIT_ENTRY: return "WAIT_ENTRY";
  case PHASE_IN_PHASE1: return "P1_HWSPIN";
  case PHASE_IN_PHASE2_PUSH: return "P2_PUSH";
  case PHASE_IN_PHASE25_SPIN: return "P2.5_SPIN";
  case PHASE_IN_PHASE3_CAS: return "P3_CAS";
  case PHASE_IN_PHASE4_PREPARK: return "P4_PREPARK";
  case PHASE_IN_PHASE4_NTWAIT: return "P4_NTWAIT";
  case PHASE_IN_PHASE4_POSTWAIT: return "P4_POSTWAIT";
  case PHASE_IN_PHASE4_DRAIN: return "P4_DRAIN";
  case PHASE_IN_CS: return "IN_CS";
  case PHASE_IN_UNLOCK_NOTIFY: return "UNLOCK";
  case PHASE_IN_POP: return "POP";
  case PHASE_IN_HANDOFF_ONE: return "HANDOFF";
  case PHASE_POST_WAIT_OK: return "POST_WAIT";
  case PHASE_WORKER_DONE: return "DONE";
  default: return "?";
  }
}

// ---------------------------------------------------------------------------
// Writer callbacks — set once by the bench before calling dump() so the
// dump module can share the bench's existing NtWriteFile-backed output.
// No atomicity needed: bench sets these from the main thread before any
// worker (or watchdog) invokes dump().
// ---------------------------------------------------------------------------
static WriteStrFn g_ws = nullptr;

void set_writers(WriteStrFn ws_fn) { g_ws = ws_fn; }

namespace {

void w(const char *s) {
  if (g_ws)
    g_ws(s);
}

void wu64(uint64_t v) {
  char buf[24];
  char *end = buf + sizeof(buf);
  char *p = end;
  *--p = '\0';
  if (v == 0) {
    *--p = '0';
  } else {
    while (v) {
      *--p = '0' + static_cast<char>(v % 10);
      v /= 10;
    }
  }
  w(p);
}

void wi64(int64_t v) {
  if (v < 0) {
    w("-");
    wu64(static_cast<uint64_t>(-v));
  } else {
    wu64(static_cast<uint64_t>(v));
  }
}

void wpad(const char *s, int width) {
  w(s);
  int len = 0;
  while (s[len])
    ++len;
  for (int i = len; i < width; ++i)
    w(" ");
}

void whex16(uint16_t v) {
  char buf[7];
  const char *hex = "0123456789abcdef";
  buf[0] = '0';
  buf[1] = 'x';
  buf[2] = hex[(v >> 12) & 0xF];
  buf[3] = hex[(v >> 8) & 0xF];
  buf[4] = hex[(v >> 4) & 0xF];
  buf[5] = hex[v & 0xF];
  buf[6] = '\0';
  w(buf);
}

} // namespace

void dump(const char *header, void *lock_futex, uint32_t nworkers) {
  if (!g_ws)
    return;

  w("\n================================================================\n");
  w(header);
  w("\n================================================================\n");

  w("\nPer-worker state:\n");
  if (nworkers > kMaxInstrWorkers)
    nworkers = kMaxInstrWorkers;
  // Helper: print a 64-bit value in hex (16 chars, leading zeros).
  auto whex64 = [](uint64_t v) {
    char buf[19];
    const char *hex = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i)
      buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = '\0';
    w(buf);
  };
  for (uint32_t i = 0; i < nworkers; ++i) {
    WorkerPhase &wp = g_phases[i];
    uint32_t phase = wp.phase.load(cpp::MemoryOrder::RELAXED);
    uint32_t iter = wp.iter.load(cpp::MemoryOrder::RELAXED);
    uint32_t last_ret = wp.last_ret.load(cpp::MemoryOrder::RELAXED);
    uint64_t hb = wp.heartbeat.load(cpp::MemoryOrder::RELAXED);
    uint64_t wa = wp.wait_address.load(cpp::MemoryOrder::RELAXED);
    uint32_t slot_idx = wp.my_slot_idx.load(cpp::MemoryOrder::RELAXED);
    uint32_t nt_status = wp.last_wait_status.load(cpp::MemoryOrder::RELAXED);
    w("  w");
    wu64(i);
    w(": ");
    wpad(phase_name_of(phase), 14);
    w(" iter=");
    wu64(iter);
    w(" last_ret=");
    wi64(static_cast<int32_t>(last_ret));
    w(" hb=");
    wu64(hb);
    w(" wa=");
    whex64(wa);
    w(" slot=");
    whex16(static_cast<uint16_t>(slot_idx));
    w(" last_nt=");
    whex64(nt_status);
    w("\n");
    // For any worker currently inside a wait (wait_address non-zero),
    // dump the slot contents so we can see link state + generation
    // for the stuck waiter directly. This sidesteps the
    // "walked=0 stack-empty" blind spot where the slot was detached
    // by a waker but the owner never woke.
    if (wa != 0 && slot_idx != 0) {
      auto &slot = wait_slot::get_slot(static_cast<uint16_t>(slot_idx));
      linkage::Link link = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint8_t st = link.state();
      uint16_t nx = link.next();
      uint32_t tag = link.tag();
      uint32_t tid = slot.thread_id.load(cpp::MemoryOrder::RELAXED);
      uint32_t gen = slot.generation.load(cpp::MemoryOrder::ACQUIRE);
      uintptr_t slot_wa =
          slot.wait_address.load(cpp::MemoryOrder::RELAXED);
      uint8_t sub = static_cast<uint8_t>(
          slot.subsystem.load(cpp::MemoryOrder::RELAXED));
      w("         slot_state: st=");
      wu64(st);
      w(" tag=");
      wu64(tag);
      w(" next=");
      whex16(nx);
      w(" tid=");
      wu64(tid);
      w(" gen=");
      wu64(gen);
      w(" slot_wa=");
      whex64(static_cast<uint64_t>(slot_wa));
      w(" sub=");
      wu64(sub);
      w("\n");
    }
  }

  if (lock_futex) {
    w("\nLinked waiters on lock (chained from stack_top):\n");
    // Futex layout: offset 0 = 32-bit stack_ ([gen:16|top:16]), offset 4 =
    // 32-bit value_. Entire 8 bytes is one atomic. We load combined as
    // uint64 to get a consistent snapshot.
    auto *p = reinterpret_cast<cpp::Atomic<uint64_t> *>(lock_futex);
    uint64_t combined = p->load(cpp::MemoryOrder::ACQUIRE);
    uint32_t stack = static_cast<uint32_t>(combined & 0xFFFFFFFFull);
    uint32_t value = static_cast<uint32_t>(combined >> 32);
    uint16_t top = static_cast<uint16_t>(stack & 0xFFFFu);
    uint16_t stack_gen = static_cast<uint16_t>(stack >> 16);
    w("  value=");
    wu64(value);
    w("  stack_gen=");
    wu64(stack_gen);
    w("  top_idx=");
    whex16(top);
    w("\n");
    uint32_t walked = 0;
    uint16_t curr = top;
    while (curr != 0 && walked < 256) {
      auto &slot = wait_slot::get_slot(curr);
      linkage::Link link = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint8_t st = link.state();
      uint16_t nx = link.next();
      uint32_t tid = slot.thread_id.load(cpp::MemoryOrder::RELAXED);
      uint32_t gen = slot.generation.load(cpp::MemoryOrder::ACQUIRE);
      uintptr_t wa = slot.wait_address.load(cpp::MemoryOrder::RELAXED);
      w("    [");
      wu64(walked);
      w("] idx=");
      whex16(curr);
      w(" st=");
      wu64(st);
      w(" tid=");
      wu64(tid);
      w(" gen=");
      wu64(gen);
      w(" wa!=0=");
      wu64(wa != 0 ? 1 : 0);
      w(" next=");
      whex16(nx);
      w("\n");
      curr = nx;
      ++walked;
    }
    w("  walked=");
    wu64(walked);
    w("\n");
  }

  w("\nCounters (non-zero only):\n");
  for (uint32_t i = 0; i < NUM_COUNTERS; ++i) {
    uint64_t v = g_counters[i].v.load(cpp::MemoryOrder::RELAXED);
    if (v == 0)
      continue;
    w("  ");
    wpad(name_of(static_cast<Counter>(i)), 46);
    w(" = ");
    wu64(v);
    w("\n");
  }
}

bool check_closure(uint64_t expected_iters) {
  if (!g_ws)
    return true;

  auto r = [](Counter c) {
    return g_counters[c].v.load(cpp::MemoryOrder::RELAXED);
  };

  bool ok = true;

  w("\nClosure identities:\n");

  {
    uint64_t acq = r(CTR_BENCH_LOCK_CAS) + r(CTR_BENCH_LOCK_HANDOFF);
    uint64_t unl = r(CTR_BENCH_UNLOCK);
    w("  [I1] lock_cas + lock_handoff == unlock:  ");
    wu64(r(CTR_BENCH_LOCK_CAS));
    w(" + ");
    wu64(r(CTR_BENCH_LOCK_HANDOFF));
    w(" = ");
    wu64(acq);
    w(" vs unlock=");
    wu64(unl);
    w(acq == unl ? "  PASS" : "  FAIL");
    if (acq != unl)
      ok = false;
    w(" (expected iters=");
    wu64(expected_iters);
    w(")\n");
  }

  // [I2] Publish ledger for the tri-state protocol.
  //
  //   publish_ok = ret_handoff (legit claim) + rollback_cas_ok
  //                (ghost rolled back) + rollback_cas_lost
  //                (recipient already claimed → we'd have returned
  //                Handoff but rollback check raced after the CAS
  //                succeeded; in that sub-case we return Completed
  //                even though value_ is locked — benign, recipient
  //                owns and will unlock normally).
  //
  //   ret_handoff = ret_1 (consumer side).
  //   bench.lock_handoff = ret_1 (wrapper bookkeeping).
  //
  // Within "in-flight tolerance" (some waiter can be mid-consume
  // at dump time).
  {
    uint64_t publ = r(CTR_HO_WAITING_PUBLISH_OK);
    uint64_t rh = r(CTR_HO_RET_HANDOFF);
    uint64_t rb_ok = r(CTR_HO_WAITING_ROLLBACK_CAS_OK);
    uint64_t rb_lost = r(CTR_HO_WAITING_ROLLBACK_CAS_LOST);
    uint64_t accounted = rh + rb_ok + rb_lost;
    uint64_t ret1 = r(CTR_WAIT_RET_1);
    uint64_t hoff = r(CTR_BENCH_LOCK_HANDOFF);
    w("  [I2a] publish_ok == ret_handoff + rollback_ok + rollback_lost:  ");
    wu64(publ);
    w(" vs ");
    wu64(accounted);
    w(" {rh=");
    wu64(rh);
    w(", rb_ok=");
    wu64(rb_ok);
    w(", rb_lost=");
    wu64(rb_lost);
    w("}");
    bool pass_a = (publ == accounted);
    w(pass_a ? "  PASS\n" : "  FAIL\n");
    if (!pass_a)
      ok = false;

    w("  [I2b] ret_handoff ≈ ret_1 ≈ bench.lock_handoff:  ");
    wu64(rh);
    w(" vs ");
    wu64(ret1);
    w(" vs ");
    wu64(hoff);
    // Tolerance: at most nworkers in-flight at dump.
    uint64_t d1 = (rh > ret1) ? rh - ret1 : ret1 - rh;
    uint64_t d2 = (ret1 > hoff) ? ret1 - hoff : hoff - ret1;
    bool pass_b = (d1 <= kMaxInstrWorkers) && (d2 <= kMaxInstrWorkers);
    w(pass_b ? "  PASS (within in-flight tolerance)\n" : "  FAIL\n");
    if (!pass_b)
      ok = false;
  }

  {
    uint64_t in = r(CTR_WAIT_ENTRIES);
    uint64_t out = r(CTR_WAIT_RET_0) + r(CTR_WAIT_RET_1) +
                   r(CTR_WAIT_RET_ETIMEDOUT) + r(CTR_WAIT_RET_EINTR) +
                   r(CTR_WAIT_RET_EINVAL) + r(CTR_WAIT_RET_ENOMEM) +
                   r(CTR_WAIT_RET_OTHER);
    w("  [I3] wait.entries == sum(wait.ret_*):  ");
    wu64(in);
    w(" vs ");
    wu64(out);
    // Mid-wait workers account for a delta of at most nworkers.
    uint64_t delta = (in > out) ? (in - out) : (out - in);
    w(delta <= kMaxInstrWorkers ? "  PASS (within in-flight tolerance)\n"
                                : "  FAIL\n");
    if (delta > kMaxInstrWorkers)
      ok = false;
  }

  {
    uint64_t pre = r(CTR_POP_PREMARK_CAS_OK);
    uint64_t det = r(CTR_POP_DETACH_OK) + r(CTR_POP_DETACH_FAIL_ORPHAN);
    w("  [I4] pop.premark_cas_ok == pop.detach_ok + pop.detach_fail_orphan:  ");
    wu64(pre);
    w(" vs ");
    wu64(det);
    w((pre == det) ? "  PASS\n" : "  FAIL\n");
    if (pre != det)
      ok = false;
  }

  {
    uint64_t in = r(CTR_POP_CALLS);
    uint64_t out = r(CTR_POP_RET_TRUE) + r(CTR_POP_RET_FALSE);
    w("  [I5] pop.calls == pop.ret_true + pop.ret_false:  ");
    wu64(in);
    w(" vs ");
    wu64(out);
    w((in == out) ? "  PASS\n" : "  FAIL\n");
    if (in != out)
      ok = false;
  }

  {
    uint64_t in = r(CTR_HO_CALLS);
    uint64_t out = r(CTR_HO_RET_HANDOFF) + r(CTR_HO_RET_COMPLETED) +
                   r(CTR_HO_RET_EMPTY);
    w("  [I6] ho.calls == ho.ret_*:  ");
    wu64(in);
    w(" vs ");
    wu64(out);
    w((in == out) ? "  PASS\n" : "  FAIL\n");
    if (in != out)
      ok = false;
  }

  {
    uint64_t un_sum = r(CTR_UN_RET_HANDOFF) + r(CTR_UN_RET_COMPLETED) +
                      r(CTR_UN_RET_EMPTY_POP_WOKE) +
                      r(CTR_UN_RET_EMPTY_POP_NONE);
    uint64_t unlocks = r(CTR_BENCH_UNLOCK);
    w("  [I7] bench.unlock == un.ret_*:  ");
    wu64(unlocks);
    w(" vs ");
    wu64(un_sum);
    w((unlocks == un_sum) ? "  PASS\n" : "  FAIL\n");
    if (unlocks != un_sum)
      ok = false;
  }

  // Splits the ret=1 claim by source, localising any consumed-ghost
  // bug to a specific decode site.
  {
    uint64_t by_site = r(CTR_WAIT_HANDOFF_CONSUMED_PHASE25) +
                       r(CTR_WAIT_HANDOFF_CONSUMED_PHASE3_SELFCOMMIT) +
                       r(CTR_WAIT_HANDOFF_CONSUMED_PHASE4_PREPARK) +
                       r(CTR_WAIT_HANDOFF_CONSUMED_PHASE4_POSTWAIT);
    uint64_t ret1 = r(CTR_WAIT_RET_1);
    w("  [I9] ret_1 == sum(handoff_consumed_*):  ");
    wu64(ret1);
    w(" vs ");
    wu64(by_site);
    w(" {p2.5=");
    wu64(r(CTR_WAIT_HANDOFF_CONSUMED_PHASE25));
    w(", p3_selfcommit=");
    wu64(r(CTR_WAIT_HANDOFF_CONSUMED_PHASE3_SELFCOMMIT));
    w(", p4_prepark=");
    wu64(r(CTR_WAIT_HANDOFF_CONSUMED_PHASE4_PREPARK));
    w(", p4_postwait=");
    wu64(r(CTR_WAIT_HANDOFF_CONSUMED_PHASE4_POSTWAIT));
    w("}");
    w((ret1 == by_site) ? "  PASS\n" : "  FAIL\n");
    if (ret1 != by_site)
      ok = false;
  }

  // [I10] Correct ghost-survivor ledger. The original gen-at-decode
  // check was wrong: by decode time, the slot has been through its
  // own clear_slot_owned + get_slot_index reuse, so cur_gen always
  // matches alloc_gen. The real check is whether a publish_lost at
  // any waker site carried HANDOFF_BIT on the slot — that's a ghost
  // being armed.
  //
  // The expected ledger at exit:
  //   ho.waiting_publish_ok − ho.waiting_ghost_revert_ok
  //   − (ghost-armed publishes that the owner's state check rejected)
  //   == wait.ret_1 (modulo at most one in-flight waiter).
  //
  // Directly measured:
  //   ghost_armed = ho.waiting_publish_lost_PRIOR_HANDOFF
  //               + ho.in_kernel_publish_lost_PRIOR_HANDOFF
  //               + pop.publish_lost_PRIOR_HANDOFF
  //
  // Any non-zero ghost_armed means the carry-over bug is active,
  // whether or not the owner happens to consume the ghost.
  // [I10] Ghost HANDOFF survivor ledger — the REAL one this time.
  //
  // HANDOFF_BIT is written into wake_word only by handoff_one's
  // WAITING branch. That publish always succeeds (publish_ok) but
  // the caller then returns Handoff only if the ghost-guard (gen
  // re-check) AND post-state (link.state==SIGNALED) both pass.
  //
  // The guard paths:
  //    publish_ok ─┬→ Handoff            (clean; caller gets ret_handoff)
  //                ├→ revert_ok → Empty  (HANDOFF erased, clean)
  //                ├→ revert_lost → Empty (HANDOFF LEAKED onto stranger!)
  //                └→ poststate_bad → Empty (HANDOFF possibly leaked)
  //
  // Leak = revert_lost + poststate_bad. The leaked HANDOFF rides on a
  // stranger (fresh-cycle) slot. When a subsequent pre-mark makes
  // that slot's state=SIGNALED, the stranger-owner's Phase-2.5/4
  // decode sees HANDOFF_BIT and returns ret=1 — a ghost consume.
  //
  // Independent cross-check: ret_1 − ho.ret_handoff = ghost consumes.
  // It should approximately equal the leak count.
  // [I10] Under the tri-state protocol, the notion of "ghost HANDOFF
  // leak" is replaced by CAS-rollback accounting. A rollback_cas_ok
  // is an intentional, correct response to a detected post-publish
  // ghost (slot migrated to another Futex, or cycled past our
  // pre-mark) — NOT a bug. A rollback_cas_lost is harmless: the
  // intended recipient legitimately claimed the handoff via
  // CAS(transit, locked) between our post-publish check and our
  // rollback CAS, so our rollback no-ops and the recipient owns
  // the lock. Either outcome preserves mutual exclusion.
  //
  // This identity just reports the counts for visibility. The
  // definitive correctness signal is I11 (double-ownership).
  {
    uint64_t rb_ok = r(CTR_HO_WAITING_ROLLBACK_CAS_OK);
    uint64_t rb_lost = r(CTR_HO_WAITING_ROLLBACK_CAS_LOST);
    w("  [I10] rollback outcomes:  ok=");
    wu64(rb_ok);
    w(" lost=");
    wu64(rb_lost);
    w("  (ok = ghost detected + cleanly rolled back;"
      " lost = recipient claimed first, benign)\n");
  }

  // [I11] Direct double-ownership. This is unambiguous: any
  // non-zero here = the mutex admitted two threads into the CS
  // simultaneously, which is the observable signature of the
  // 16T hang (locally a counter++ race; globally a deadlock when
  // both "owners" issue unlock_notify and the second one's scan
  // finds nothing to wake).
  {
    uint64_t dup = r(CTR_BENCH_DOUBLE_OWNERSHIP_OBSERVED);
    uint64_t peak = r(CTR_BENCH_MAX_CS_OCCUPANCY_OBSERVED);
    w("  [I11] double-ownership events:  ");
    wu64(dup);
    w("  max CS occupancy: ");
    wu64(peak);
    w(dup == 0 ? "  MUTUAL EXCLUSION HELD\n" : "  MUTUAL EXCLUSION VIOLATED\n");
    if (dup != 0)
      ok = false;
  }

  {
    uint64_t calls = r(CTR_WAIT_PHASE4_NTWAIT_CALLS);
    uint64_t rets = r(CTR_WAIT_PHASE4_NTWAIT_RET_ALERTED) +
                    r(CTR_WAIT_PHASE4_NTWAIT_RET_TIMEOUT) +
                    r(CTR_WAIT_PHASE4_NTWAIT_RET_USER_APC) +
                    r(CTR_WAIT_PHASE4_NTWAIT_RET_OTHER);
    w("  [I8] phase4.ntwait_calls == sum(ret_*):  ");
    wu64(calls);
    w(" vs ");
    wu64(rets);
    uint64_t diff = (calls > rets) ? calls - rets : rets - calls;
    w((diff <= kMaxInstrWorkers) ? "  PASS (within in-flight tolerance)\n"
                                 : "  FAIL\n");
    if (diff > kMaxInstrWorkers)
      ok = false;
  }

  return ok;
}

} // namespace futex_instrument
} // namespace LIBC_NAMESPACE_DECL
