//===--- Parking lot for address-keyed waits + shared utilities --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Address-keyed parking lot — 256 cache-line-aligned buckets backing
// futex_addr::wait/wake on arbitrary memory addresses. Per-bucket
// spinlocks with locked insert + Dekker re-check. Shares the
// wait_slot pool and sleep/wake primitives with futex_utils.
//
// Wake mirrors futex_utils' fold: slot.link's state byte carries
// both bucket-SLL lifecycle and the owner-visible wake. One
// link_cas_snap atomically transitions WAITING/IN_KERNEL →
// SIGNALED_CLEAN under the bucket lock. sll_remove runs under the
// same lock before the owner's next observation, so every parking-
// lot wake is CLEAN — the SIGNALED_*_ORPHAN encodings from
// futex_utils don't apply.
//
// Owner observes slot.link's state byte (never a separate wake
// word), so a signal APC racing with a pre-mark reaches the
// Interruptible EINTR branch instead of being misreported as a
// normal wake. Alert TIDs are captured into a stack-local buffer
// under the lock — never chained through slot.link.next — so the
// owner's cleanup has no window to truncate the alert walk.
//
// Provides: cpu_relax / spin / timeout helpers, parking-lot
// internals (Bucket / BucketLockHolder / hash / wait / wake),
// owner-side clear_slot_owned and absorb_premark_wake.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_ADDR_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_ADDR_H

#include "hdr/types/struct_timespec.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/wait_slot.h"

#include <errno.h>

namespace LIBC_NAMESPACE_DECL {
namespace futex_addr {

//===----------------------------------------------------------------------===//
// Architecture-portable spin hint
//===----------------------------------------------------------------------===//

LIBC_INLINE void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__ volatile("yield" ::: "memory");
#else
  __asm__ volatile("" ::: "memory");
#endif
}

//===----------------------------------------------------------------------===//
// Parking lot (address-keyed waits)
//===----------------------------------------------------------------------===//

static constexpr unsigned BUCKET_COUNT = 256;

// Adaptive TTAS spinlock.
//   Phase 1: single exchange (fast uncontended path).
//   Phase 2: read-spin keeps the line Shared, budget doubles 4→128.
//   Phase 3: NtYieldExecution after each exhausted budget — avoids
//            SMT-core starvation under sustained contention.
struct BucketLock {
  cpp::Atomic<uint8_t> flag{0};

  LIBC_INLINE void acquire() {
    if (flag.exchange(1, cpp::MemoryOrder::ACQUIRE) == 0)
      return;
    // Quick retry before paying the full TTAS loop's setup.
    for (int retry = 0; retry < 2; ++retry) {
      cpu_relax();
      if (flag.exchange(1, cpp::MemoryOrder::ACQUIRE) == 0)
        return;
    }
    for (unsigned outer = 0;; ++outer) {
      unsigned spin_limit = 4u << (outer < 5 ? outer : 5);
      bool saw_unlocked = false;
      for (unsigned i = 0; i < spin_limit; ++i) {
        if (flag.load(cpp::MemoryOrder::RELAXED) == 0) {
          saw_unlocked = true;
          break;
        }
        cpu_relax();
      }
      if (!saw_unlocked)
        ::NtYieldExecution();
      if (flag.exchange(1, cpp::MemoryOrder::ACQUIRE) == 0)
        return;
    }
  }

  LIBC_INLINE void release() {
    flag.store(0, cpp::MemoryOrder::RELEASE);
  }
};

// Cache-line aligned to avoid false sharing between adjacent
// buckets. live_count is exact (incremented on insert under lock;
// decremented on wake claim under lock OR timeout CAS lock-free)
// and enables the lock-free empty-bucket fast path — wakers skip
// the lock entirely when zero.
//
// SLL: head = oldest waiter, tail = newest. Inserts append at tail
// (O(1) via tail pointer); wake walks head→tail and claims the
// first `count` matches in FIFO order in a single pass.
struct Bucket; // forward decl for BucketLockHolder friendship

// Typed proof that bucket.lock is held. Constructible only by
// Bucket::acquire_held(); released by ~BucketLockHolder() or
// explicit release(). Bucket-mutating helpers take
// BucketLockHolder& so "lock held" is a type-level precondition.
//
// Layered (vs RAII guard) because:
//   - Wake releases the lock mid-function and continues with the
//     batch alert flush. RAII alone (always-release-on-scope-exit)
//     doesn't fit.
//   - Some helpers MUST run under the lock and others MUST NOT —
//     the typed parameter catches misuse at compile time.
//
// Movable but not copyable. dtor releases if not already released;
// explicit release() is preferred so the release point is visible
// at the call site.
struct BucketLockHolder {
  BucketLockHolder(const BucketLockHolder &) = delete;
  BucketLockHolder &operator=(const BucketLockHolder &) = delete;
  LIBC_INLINE BucketLockHolder(BucketLockHolder &&other) noexcept
      : b_(other.b_) {
    other.b_ = nullptr;
  }
  BucketLockHolder &operator=(BucketLockHolder &&) = delete;

  LIBC_INLINE ~BucketLockHolder();

  // Idempotent (no-op after a prior release()).
  LIBC_INLINE void release();

  // For helpers needing the bucket itself (sll_insert reading
  // bucket.head/tail).
  LIBC_INLINE Bucket &bucket() const { return *b_; }

private:
  Bucket *b_; // nullptr after release; set by Bucket::acquire_held().
  friend struct Bucket;
  LIBC_INLINE explicit BucketLockHolder(Bucket &b) : b_(&b) {}
};

struct alignas(64) Bucket {
  BucketLock lock;
  uint32_t head{0}; // oldest waiter (SLL head), protected by lock
  uint32_t tail{0}; // newest waiter (SLL tail), protected by lock
  cpp::Atomic<uint32_t> live_count{0};

  // [[nodiscard]] catches "lock and immediately unlock on
  // destruction" — caller MUST bind the holder to a name.
  [[nodiscard]] LIBC_INLINE BucketLockHolder acquire_held() {
    lock.acquire();
    return BucketLockHolder{*this};
  }
};

LIBC_INLINE BucketLockHolder::~BucketLockHolder() {
  if (b_)
    b_->lock.release();
}

LIBC_INLINE void BucketLockHolder::release() {
  if (b_) {
    b_->lock.release();
    b_ = nullptr;
  }
}

inline Bucket &get_bucket(unsigned index) {
  static Bucket buckets[BUCKET_COUNT];
  return buckets[index];
}

LIBC_INLINE unsigned hash_address(uintptr_t addr) {
  addr >>= 4;
  addr ^= addr >> 16;
  addr ^= addr >> 8;
  return static_cast<unsigned>(addr) & (BUCKET_COUNT - 1);
}

//===----------------------------------------------------------------------===//
// DLL operations (all called under bucket lock)
//===----------------------------------------------------------------------===//

// All SLL mutations run under bucket.lock, so RELAXED suffices —
// lock acquire/release provides publication. BucketLockHolder& is
// the typed precondition; the bucket is recovered via held.bucket().

LIBC_INLINE void sll_insert(BucketLockHolder &held, uint32_t idx) {
  Bucket &bucket = held.bucket();
  auto &slot = wait_slot::get_slot(idx);
  // Slot is transitioning on-chain — clear CERT so on-chain
  // residency reads CERT=0 (truthful). The inserting slot is
  // owner-exclusive (we just primed its link), so the non-CAS
  // link_store_next_uncertify is safe here.
  linkage::link_store_next_uncertify(slot.link, wait_slot::NULL_INDEX);
  if (bucket.tail == wait_slot::NULL_INDEX) {
    bucket.head = idx;
  } else {
    // Old tail's owner can be concurrently CAS-ing its state
    // (Phase 3 WAITING→IN_KERNEL) without holding the bucket
    // lock — must use link_cas_next to preserve that transition.
    linkage::link_cas_next(wait_slot::get_slot(bucket.tail).link,
                              static_cast<uint16_t>(idx));
  }
  bucket.tail = idx;
}

LIBC_INLINE void sll_remove(BucketLockHolder &held, uint32_t idx) {
  Bucket &bucket = held.bucket();
  uint32_t next_of_idx = linkage::link_next(
      wait_slot::get_slot(idx).link.load(cpp::MemoryOrder::RELAXED));
  if (bucket.head == idx) {
    bucket.head = next_of_idx;
    if (bucket.tail == idx)
      bucket.tail = wait_slot::NULL_INDEX;
    return;
  }
  uint32_t prev = bucket.head;
  while (prev != wait_slot::NULL_INDEX) {
    uint32_t next = linkage::link_next(
        wait_slot::get_slot(prev).link.load(cpp::MemoryOrder::RELAXED));
    if (next == idx) {
      // prev's owner may be concurrently CAS-ing its state
      // (Phase 3 WAITING→IN_KERNEL) outside the bucket lock —
      // CAS-loop the .next rewrite to preserve that transition.
      linkage::link_cas_next(wait_slot::get_slot(prev).link,
                                static_cast<uint16_t>(next_of_idx));
      if (bucket.tail == idx)
        bucket.tail = prev;
      return;
    }
    prev = next;
  }
}

//===----------------------------------------------------------------------===//
// Timeout conversion
//===----------------------------------------------------------------------===//

LIBC_INLINE LARGE_INTEGER *
timespec_to_nt(const struct timespec *timeout, LARGE_INTEGER &storage) {
  if (!timeout)
    return nullptr;
  long long hns = static_cast<long long>(timeout->tv_sec) * 10'000'000LL +
                  timeout->tv_nsec / 100;
  // Clamp negative (bogus) to 1 tick — prevents infinite waits.
  if (hns < 0)
    hns = 1;
  storage.QuadPart = -hns; // NT relative timeout is negative
  return &storage;
}

// Capture absolute monotonic deadline from a relative NT timeout.
// 0 ⇒ no deadline (infinite wait). Mirrors futex_utils' Timeout
// type folded into a single ULONGLONG: lets the Phase 4 retry loop
// refresh the relative-from-now nt_timeout against the same anchor
// instead of restarting "full duration" on every reloop (which
// silently extends the wait past the user's deadline under stray
// alert pressure).
LIBC_INLINE ULONGLONG capture_deadline_hns(const LARGE_INTEGER *nt_timeout) {
  if (!nt_timeout)
    return 0;
  ULONGLONG now_hns;
  ::RtlQueryUnbiasedInterruptTime(&now_hns);
  long long relative_hns = -nt_timeout->QuadPart;
  if (relative_hns < 0)
    relative_hns = 0;
  return now_hns + static_cast<ULONGLONG>(relative_hns);
}

// Recompute relative-from-now into storage. Returns nullptr when no
// deadline was set (infinite wait). storage.QuadPart == -1 sentinel
// ⇒ deadline already past; caller bails to -ETIMEDOUT. Mirrors
// futex_utils::timeout_to_nt at futex_utils.h:1330.
LIBC_INLINE LARGE_INTEGER *
deadline_to_nt(ULONGLONG deadline_hns, LARGE_INTEGER &storage) {
  if (deadline_hns == 0)
    return nullptr;
  ULONGLONG now_hns;
  ::RtlQueryUnbiasedInterruptTime(&now_hns);
  if (now_hns >= deadline_hns) {
    storage.QuadPart = -1;
    return &storage;
  }
  long long remaining = static_cast<long long>(deadline_hns - now_hns);
  storage.QuadPart = -remaining;
  return &storage;
}

// Deferred stale-slot reclaim. Timeout cancellation is lock-free
// (CAS IN_KERNEL→TIMED_OUT); the slot stays linked until the next
// wake scan or this call lazily cleans it. Deferred from the
// latency-sensitive timeout path to wait-entry.

LIBC_INLINE void reclaim_stale_slot() {
  uintptr_t stale_addr;
  uint32_t expected_gen;
  uint32_t stale_idx = wait_slot::get_stale_slot(stale_addr, expected_gen);
  if (stale_idx == wait_slot::NULL_INDEX)
    return;

  unsigned bi = hash_address(stale_addr);
  Bucket &b = get_bucket(bi);
  BucketLockHolder held = b.acquire_held();
  auto &slot = wait_slot::get_slot(stale_idx);
  // Re-check under lock — a wake scan may have already reclaimed
  // and recycled the slot. Gen + state==TIMED_OUT both required;
  // otherwise the slot belongs to a different live waiter.
  if (slot.generation.load(cpp::MemoryOrder::ACQUIRE) == expected_gen &&
      linkage::link_load_state(slot.link, cpp::MemoryOrder::RELAXED) ==
          wait_slot::TIMED_OUT) {
    sll_remove(held, stale_idx);
    // live_count already decremented by the timeout path.
    slot.subsystem.store(wait_slot::SubsystemKind::None,
                          cpp::MemoryOrder::RELAXED);
    // sll_remove proved off-chain — link_store sets CERT (vs
    // link_store_state which only preserves).
    linkage::link_store(slot.link, wait_slot::IDLE,
                           wait_slot::NULL_INDEX);
    slot.wait_address.store(0, cpp::MemoryOrder::RELAXED);
  }
}

// Recover the parking-lot user address gated on subsystem tag.
// Symmetric to Futex::try_from_owner_slot — the ONLY blessed cast
// for SubsystemKind::ParkingLot-tagged wait_address from caller
// code. The parking-lot trampoline below is the one other cast,
// type-safe by registration.
LIBC_INLINE void *
try_user_addr_from_owner_slot(wait_slot::WaitSlot &slot) {
  if (slot.subsystem.load(cpp::MemoryOrder::ACQUIRE) !=
      wait_slot::SubsystemKind::ParkingLot)
    return nullptr;
  return reinterpret_cast<void *>(
      slot.wait_address.load(cpp::MemoryOrder::RELAXED));
}

// Thread-exit / stale-slot unlink, registered with wait_slot at
// startup. Called by slot_cleanup (thread exit) and by Futex::wait
// Phase 1.75 (TLS holds a SubsystemKind::ParkingLot slot).
//
// Under bucket.lock:
//   1. CAS WAITING/IN_KERNEL → TIMED_OUT. Win ⇒ this thread owns
//      cleanup: live_count-- AND sll_remove.
//   2. Already TIMED_OUT (prior timeout never cleaned): live_count
//      already decremented by timeout path; only sll_remove needed.
//   3. SIGNALED: waker already unlinked; nothing to do.
//
// wait_address re-checked under lock (TOCTOU defense). Returns
// true iff the caller should reclaim_slot(idx). Single-actor
// reclaim — at most one concurrent caller sees true per lifecycle.
LIBC_INLINE bool
parking_lot_unlink_thread_exit(void *wait_addr_as_void, uint16_t idx,
                               uint32_t expected_gen) {
  if (!wait_addr_as_void)
    return false;
  uintptr_t wa = reinterpret_cast<uintptr_t>(wait_addr_as_void);
  unsigned bi = hash_address(wa);
  Bucket &b = get_bucket(bi);
  BucketLockHolder held = b.acquire_held();
  auto &slot = wait_slot::get_slot(idx);
  // Gen check under the lock: if the slot has been reclaimed (and
  // possibly reallocated to a different user addr or thread), refuse.
  if (slot.generation.load(cpp::MemoryOrder::ACQUIRE) != expected_gen)
    return false; // held's dtor releases lock
  if (slot.wait_address.load(cpp::MemoryOrder::RELAXED) != wa)
    return false; // slot was reused since caller read wait_address

  uint8_t st = linkage::link_load_state(slot.link,
                                           cpp::MemoryOrder::RELAXED);
  bool need_live_dec = false;
  bool need_unlink = false;
  bool reclaim = false;
  if (st == wait_slot::WAITING) {
    if (linkage::link_cas_state<wait_slot::WAITING,
                                    wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
      need_live_dec = true;
      need_unlink = true;
      reclaim = true;
    } else if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                            wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(
                   slot.link)) {
      need_live_dec = true;
      need_unlink = true;
      reclaim = true;
    }
  } else if (st == wait_slot::IN_KERNEL) {
    if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                    wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
      need_live_dec = true;
      need_unlink = true;
      reclaim = true;
    }
  } else if (st == wait_slot::TIMED_OUT) {
    // Prior timeout already decremented live_count.
    need_unlink = true;
    reclaim = true;
  }
  // SIGNALED: waker already unlinked + will reclaim. reclaim=false.

  if (need_unlink)
    sll_remove(held, idx);
  if (need_live_dec)
    b.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
  return reclaim;
}

//===----------------------------------------------------------------------===//
// Owner-side slot cleanup / pre-mark absorption (mirrors futex_utils)
//===----------------------------------------------------------------------===//

// Owner-side cleanup after observing a wake. Under the fold the
// waker's single link_cas_snap (WAITING/IN_KERNEL → SIGNALED_CLEAN)
// happened under bucket.lock and atomically published the wake AND
// removed us from the SLL — the slot is off-chain and ours.
//
// Order: subsystem first (racing thread-exit trampoline skips us);
// link→IDLE; wait_address last. RELAXED — the waker's RELEASE on
// slot.link already synchronizes with our post-wake reads.
LIBC_INLINE void clear_slot_owned(wait_slot::WaitSlot &slot) {
  slot.subsystem.store(wait_slot::SubsystemKind::None,
                        cpp::MemoryOrder::RELAXED);
  linkage::link_store(slot.link, wait_slot::IDLE, wait_slot::NULL_INDEX);
  slot.wait_address.store(0, cpp::MemoryOrder::RELAXED);
  // Restore "match any M_k" default for classical FUTEX_WAIT reuse
  // without the caller re-setting.
  slot.wake_bitset = 0xFFFFFFFFu;
}

// Drain a waker's pending alert when the owner's IN_KERNEL→TIMED_OUT
// CAS lost to a racing link_cas_snap → SIGNALED_CLEAN. The fold's
// pre-mark issued an unconditional alert; drain it so it doesn't
// latch for a future alertable wait. Drain miss (waker preempted
// pre-syscall or killed post-publish) sets expect_late_alert so
// Phase 4's next STATUS_ALERTED + IN_KERNEL classifies stale.
LIBC_INLINE void drain_waker_alert(PVOID tid_ptr) {
  static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
  NTSTATUS drain_status =
      ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
  if (drain_status != STATUS_ALERTED)
    wait_slot::mark_expect_late_alert();
}

//===----------------------------------------------------------------------===//
// Wait (address-keyed, parking lot)
//===----------------------------------------------------------------------===//

template <typename T = uint32_t, bool Interruptible = false>
inline long wait_nt(const volatile T *addr, T expected,
                    LARGE_INTEGER *nt_timeout,
                    uint32_t bitset = 0xFFFFFFFFu) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 ||
                sizeof(T) == 4 || sizeof(T) == 8,
                "futex_addr::wait requires 1, 2, 4, or 8 byte type");

  if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected)
    return -EAGAIN;

  // Capture absolute monotonic deadline ONCE. The Phase 4 retry loop
  // refreshes relative-from-now into a local LARGE_INTEGER against
  // this anchor — closes the "stray alert resets the deadline" leak
  // that would silently extend the wait past the user's timeout.
  // 0 ⇒ infinite wait (caller passed nullptr).
  ULONGLONG deadline_hns = capture_deadline_hns(nt_timeout);

  // Phase 1: hardware-monitored value spin. UMWAIT C0.2 / MWAITX on the
  // futex cache line — near-zero power, wakes instantly on any write.
  // Same budget as Futex::wait Phase 1 (threshold × 1024 TSC ticks).
  if (spin_wait::spin_on_raw(const_cast<T *>(addr), expected))
    return -EAGAIN;

  if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected)
    return -EAGAIN;

  // Phase 1.5: timeout already expired post-Phase-1? Bail before
  // paying slot alloc + bucket lock + Phase 2/2.5/3 + kernel
  // round-trip. Mirrors futex_utils::wait_one_cycle Phase 1.5.
  if (deadline_hns) {
    LARGE_INTEGER probe;
    LARGE_INTEGER *p = deadline_to_nt(deadline_hns, probe);
    if (p && p->QuadPart == -1)
      return -ETIMEDOUT;
  }

  reclaim_stale_slot();

  uint32_t my_idx = wait_slot::get_slot_index();
  if (my_idx == wait_slot::NULL_INDEX)
    return -EAGAIN;

  auto &slot = wait_slot::get_slot(my_idx);
  // Prime for a fresh wait. State byte is the wake signal — the
  // link_store WAITING + tag bump is enough; any prior-cycle waker
  // with a stale snap fails its link_cas_snap on tag+state mismatch.
  slot.wait_address.store(reinterpret_cast<uintptr_t>(addr),
                            cpp::MemoryOrder::RELAXED);
  // wake_bitset: written BEFORE the bucket-lock SLL insert while
  // TLS-exclusive; wakers read under the same lock, so bucket.lock
  // release/acquire provides publication.
  slot.wake_bitset = bitset;
  linkage::link_store(slot.link, wait_slot::WAITING, wait_slot::NULL_INDEX);
  // Tag as parking-lot owned for thread-exit dispatch.
  slot.subsystem.store(wait_slot::SubsystemKind::ParkingLot,
                       cpp::MemoryOrder::RELAXED);

  unsigned bucket_idx = hash_address(reinterpret_cast<uintptr_t>(addr));
  Bucket &bucket = get_bucket(bucket_idx);

  // Insert under lock + Dekker re-check.
  //
  // Dekker pairing (LOAD-BEARING):
  //   Waker:  value.store(X, SC); SC fence; live_count.load(SC).
  //   Waiter: live_count.fetch_add(1, SC); load(addr, ACQUIRE).
  //
  // The fetch_add MUST be SEQ_CST. On x86 RELAXED RMW is
  // LOCK-prefixed and de-facto SC, so RELAXED appeared to work; on
  // ARM64 RELAXED fetch_add compiles to LDADD without -AL — NOT in
  // the SC order — and the waker can observe live_count==0 while
  // the waiter observes the old value. Upgrade to SEQ_CST costs one
  // LDADDAL vs LDADD and closes the pairing on all architectures.
  //
  // The fetch_sub on undo stays RELAXED: same critical section as
  // the sll_remove that makes the slot unreachable, no cross-thread
  // visibility order needed.
  {
    BucketLockHolder held = bucket.acquire_held();
    bucket.live_count.fetch_add(1, cpp::MemoryOrder::SEQ_CST);
    sll_insert(held, my_idx);
    // Re-check value under lock — undo immediately on mismatch
    // (no dead node).
    if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected) {
      sll_remove(held, my_idx);
      bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
      held.release(); // explicit release before owner-exclusive cleanup
      clear_slot_owned(slot);
      return -EAGAIN;
    }
    // dtor releases the lock at scope exit (before kernel sleep).
  }

  PVOID tid_ptr =
      reinterpret_cast<PVOID>(static_cast<uintptr_t>(
          slot.thread_id.load(cpp::MemoryOrder::RELAXED)));

  // Phase 2.5: pre-kernel cache spin. Waker pre-marks → SIGNALED_CLEAN
  // under bucket lock AFTER sll_remove, so any non-WAITING state
  // here guarantees off-chain.
  //
  // ALERT_FIRED check kept uniform with every SIGNALED-observation
  // site (see Futex::wait_one_cycle Phase 2.5). Currently a no-op:
  // futex_addr::wake only alerts IN_KERNEL pre-marks (gated on
  // prev_state == IN_KERNEL), so a WAITING pre-mark — the only
  // kind Phase 2.5 catches — carries ALERT_FIRED=0 by structural
  // invariant. Future refactors that introduce an IN_KERNEL pre-
  // mark route here will behave correctly.
  if (spin_wait::spin_on_link_state(&slot.link, wait_slot::WAITING)) {
    linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
    if (snap.is_alert_fired())
      wait_slot::mark_expect_late_alert();
    clear_slot_owned(slot);
    return 0;
  }

  // Phase 3: WAITING → IN_KERNEL. CAS fail ⇒ waker pre-marked
  // SIGNALED_CLEAN between Phase 2.5 exit and here. Pre-mark snap
  // was WAITING (CAS expected WAITING) → ALERT_FIRED=0 by the same
  // invariant as Phase 2.5; bit check kept uniform.
  if (!linkage::link_cas_state<wait_slot::WAITING,
                                    wait_slot::IN_KERNEL, wait_slot::WaitSlotStateTraits>(slot.link)) {
    linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
    if (snap.is_alert_fired())
      wait_slot::mark_expect_late_alert();
    clear_slot_owned(slot);
    return 0;
  }

  // Phase 4: kernel sleep. Stale-latched-alert classification uses
  // the per-thread expect_late_alert flag, set at SIGNALED-
  // observation sites whose link snap carries LINK_ALERT_FIRED_BIT
  // (waker pre-marked at IN_KERNEL → alert fired) and at drain
  // misses. See wait_slot.h.
  //
  // nt_timeout_ptr is recomputed against deadline_hns on every retry
  // path. Mirrors futex_utils Phase 4: nt_storage is owned locally;
  // the caller's nt_timeout LARGE_INTEGER is never mutated. On
  // first iteration the freshly-converted relative timeout is
  // effectively the same as the caller passed; subsequent iterations
  // shrink against the absolute anchor so the wait can't exceed the
  // user's deadline under stray-alert pressure.
  LARGE_INTEGER nt_storage;
  LARGE_INTEGER *nt_timeout_ptr = deadline_to_nt(deadline_hns, nt_storage);

  long ret = 0;
  for (;;) {
    // Pre-wait state check: waker may have pre-marked between our
    // Phase 3 commit and here. ALERT_FIRED on the snap signals that
    // the pre-mark was at IN_KERNEL (alert is in flight) — set the
    // flag so the next NtWait classifies the latched alert stale.
    {
      linkage::Link snap = slot.link.load(cpp::MemoryOrder::ACQUIRE);
      uint8_t st = snap.state();
      if (wait_slot::state_is_signaled(st)) {
        if (snap.is_alert_fired())
          wait_slot::mark_expect_late_alert();
        ret = 0;
        break;
      }
    }

    NTSTATUS status = ::NtWaitForAlertByThreadId(tid_ptr, nt_timeout_ptr);

    {
      uint8_t st = linkage::link_load_state(slot.link);
      if (wait_slot::state_is_signaled(st)) {
        if (status != STATUS_ALERTED) {
          // Wakeup came from a different source; drain the waker's
          // pending alert so it doesn't latch.
          drain_waker_alert(tid_ptr);
        }
        ret = 0;
        break;
      }
    }

    if (status == STATUS_TIMEOUT) {
      // Lock-free timeout: CAS IN_KERNEL → TIMED_OUT. Only one of
      // {timeout, wake claim} can win per slot — no double-decrement.
      if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                       wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
        bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
        ret = -ETIMEDOUT;
        break;
      }
      // Waker raced our cancel.
      drain_waker_alert(tid_ptr);
      ret = 0;
      break;
    }

    if (wait_slot::classify_stale_alert(status)) {
      // Stale alert. Re-verify the caller's predicate before reloop:
      // if *addr moved off `expected`, our wait no longer holds —
      // cancel + return -EAGAIN (matches Phase 1's value-mismatch).
      if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected) {
        if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                        wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
          bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
          ret = -EAGAIN;
          break;
        }
        drain_waker_alert(tid_ptr);
        ret = 0;
        break;
      }
      // Refresh nt_timeout_ptr against the absolute deadline before
      // re-entering NtWait — without this, a stray-alert-driven
      // reloop would restart "full duration" each pass and the wait
      // could outlive the user's timeout indefinitely.
      nt_timeout_ptr = deadline_to_nt(deadline_hns, nt_storage);
      if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1) {
        if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                        wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
          bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
          ret = -ETIMEDOUT;
          break;
        }
        drain_waker_alert(tid_ptr);
        ret = 0;
        break;
      }
      continue;
    }

    // Signal-style wake (STATUS_USER_APC or stray STATUS_ALERTED).
    // The latter shouldn't happen under the fold (waker's pre-mark
    // CAS precedes the alert) — defensively treated as signal.
    if constexpr (Interruptible) {
      if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                       wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
        bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
        ret = -EINTR;
        break;
      }
      drain_waker_alert(tid_ptr);
      ret = 0;
      break;
    }
    // Non-interruptible: refresh nt_timeout_ptr and re-sleep. Same
    // rationale as the stale-alert reloop above — the absolute
    // deadline anchors the retry against the user's original
    // timeout.
    nt_timeout_ptr = deadline_to_nt(deadline_hns, nt_storage);
    if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1) {
      if (linkage::link_cas_state<wait_slot::IN_KERNEL,
                                      wait_slot::TIMED_OUT, wait_slot::WaitSlotStateTraits>(slot.link)) {
        bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
        ret = -ETIMEDOUT;
        break;
      }
      drain_waker_alert(tid_ptr);
      ret = 0;
      break;
    }
  }

  // SIGNALED: waker already unlinked.
  // TIMED_OUT/EINTR: slot stays SLL-linked as TIMED_OUT until
  // reclaim_stale_slot / thread-exit / next wake scan.
  if (ret != -ETIMEDOUT && ret != -EINTR)
    clear_slot_owned(slot);
  return ret;
}

template <typename T = uint32_t, bool Interruptible = false>
inline long wait_nt(const volatile cpp::Atomic<T> *addr, T expected,
                    LARGE_INTEGER *nt_timeout,
                    uint32_t bitset = 0xFFFFFFFFu) {
  return wait_nt<T, Interruptible>(&addr->val, expected, nt_timeout, bitset);
}

template <typename T = uint32_t, bool Interruptible = false>
inline long wait(const volatile T *addr, T expected,
                 const struct timespec *timeout,
                 uint32_t bitset = 0xFFFFFFFFu) {
  LARGE_INTEGER nt_storage;
  LARGE_INTEGER *nt_timeout = timespec_to_nt(timeout, nt_storage);
  return wait_nt<T, Interruptible>(addr, expected, nt_timeout, bitset);
}

template <typename T = uint32_t, bool Interruptible = false>
inline long wait(const volatile cpp::Atomic<T> *addr, T expected,
                 const struct timespec *timeout,
                 uint32_t bitset = 0xFFFFFFFFu) {
  LARGE_INTEGER nt_storage;
  LARGE_INTEGER *nt_timeout = timespec_to_nt(timeout, nt_storage);
  return wait_nt<T, Interruptible>(addr, expected, nt_timeout, bitset);
}

//===----------------------------------------------------------------------===//
// Wake (address-keyed, parking lot)
//===----------------------------------------------------------------------===//
//
// Single-pass walk head→tail under bucket.lock. Per slot:
//   TIMED_OUT  inline unlink, continue.
//   Non-match  skip.
//   Match      pre-mark CAS state → SIGNALED_CLEAN. On success,
//              sll_remove + capture TID into stack-local alert buffer
//              (IN_KERNEL only), continue. CAS fail ⇒ leave for next
//              wake / reclaim_stale_slot / thread-exit.
// Stop when woken == count.
//
// Per slot, under lock:
//   [B] link_cas_state → SIGNALED_CLEAN (single-atomic publish;
//       CLEAN is correct because sll_remove below runs under the
//       same lock, so the slot is retired atomically from any
//       observer's view).
//   [R] sll_remove.
//   [C] Capture slot.thread_id if prev was IN_KERNEL (WAITING
//       catches wake via Phase 2.5 cache spin).
// Flush alert_multiple under lock when the batch fills; flush tail
// after unlock.
//
// FIFO: inserts append at tail; head→tail walk visits in arrival
// order, so the first `count` successful CAS-claims are the oldest
// `count` live matches.
//
// CAPTURE TIDs UNDER LOCK — never via slot.link.next post-unlock.
// The old design overloaded slot.link.next as a post-lock alert
// chain; a timed-out-but-pre-marked IN_KERNEL owner could clean up
// and reset its next mid-walk, truncating the chain. Stack-local
// scalar capture eliminates that window.

// wake_mask: FUTEX_WAKE_BITSET semantics. Eligible iff
// (slot.wake_bitset & wake_mask) != 0. Fixed for the whole walk.
// Classic FUTEX_WAKE callers pass ~0u (matches default ~0u). Mask
// of 0 wakes nothing — caller is responsible for rejecting upstream
// if the spec requires (Linux returns EINVAL from FUTEX_WAKE_BITSET).
inline long wake(const volatile void *addr, uint32_t count,
                 uint32_t wake_mask = 0xFFFFFFFFu) {
  if (count == 0)
    return 0;

  uintptr_t target = reinterpret_cast<uintptr_t>(addr);
  unsigned bucket_idx = hash_address(target);
  Bucket &bucket = get_bucket(bucket_idx);

  // Dekker fence: ensure caller's value-store is globally visible
  // before we read live_count (pairs with waiter's lock-held SC
  // fetch_add).
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  uint32_t hint_live =
      bucket.live_count.load(cpp::MemoryOrder::SEQ_CST);
  if (hint_live == 0)
    return 0;

  // Allocate scratch BEFORE bucket.acquire_held — no alloc work
  // under the lock. ScratchAlloc on the WAKER's ThreadScratch
  // (single-owner, lazy); null only on catastrophic OOM at process-
  // dying levels (assert; any fallback is equally doomed).
  //
  // Size = min(count, hint_live + 64). The +64 absorbs churn
  // between live_count load and lock acquire. Overflow falls
  // through to per-slot alert_one (graceful degradation for
  // "live_count grew by >64 between load and lock").
  static constexpr uint32_t SCRATCH_CAP = 8192; // 64 KB per arena slice
  uint32_t want =
      count < hint_live + 64 ? count : (hint_live + 64);
  if (want > SCRATCH_CAP)
    want = SCRATCH_CAP;

  // BatchTarget (AOS: ref+tid+slot_idx) needed because the
  // parking-lot pre-mark CAS doesn't own a state-transition exchange
  // — alert-time validation closes the stale-TID alert-leak.
  internal::ScratchAlloc<wait_slot::BatchTarget> tgt_scratch(want);
  internal::ScratchAlloc<HANDLE> out_scratch(want);
  LIBC_ASSERT(static_cast<bool>(tgt_scratch) &&
              static_cast<bool>(out_scratch));
  wait_slot::BatchTarget *tgt_buf = tgt_scratch.data();
  HANDLE *out_buf = out_scratch.data();
  uint32_t scratch_cap = static_cast<uint32_t>(tgt_scratch.size());
  uint32_t scratch_used = 0;

  long woken = 0;

  // AutoBoost key = wait-key address (matches slot.wait_address;
  // wait-chain analysis tools correlate pre-mark/detach/alert on
  // one key).
  PS_ALERT_THREAD_EXTENDED_PARAMETER ab_ctx{};
  ab_ctx.Pointer = reinterpret_cast<void *>(target);

  // Explicit release after the walk so the alert-flush syscall
  // doesn't run under the lock.
  BucketLockHolder held = bucket.acquire_held();

  uint32_t prev_idx = wait_slot::NULL_INDEX;
  uint32_t curr = bucket.head;
  while (curr != wait_slot::NULL_INDEX &&
         static_cast<uint32_t>(woken) < count) {
    auto &slot = wait_slot::get_slot(curr);
    linkage::Link slot_link = slot.link.load(cpp::MemoryOrder::RELAXED);
    uint32_t next = slot_link.next();
    uint8_t st = slot_link.state();
    // Prefetch next slot's line (write-intent: most paths below CAS
    // or store into it).
    if (next != wait_slot::NULL_INDEX)
      __builtin_prefetch(&wait_slot::get_slot(next), 1, 3);

    // Inline-clean lock-free-timeout dead entries.
    // DO NOT reclaim_slot here — parking-lot slots are TLS-owned
    // and a gen bump would race the owner's TLS reuse. Owner picks
    // it up via its (idx, gen) TLS pair, or thread-exit
    // slot_cleanup's IDLE fast path returns it to the freelist.
    if (st == wait_slot::TIMED_OUT) {
      if (prev_idx != wait_slot::NULL_INDEX)
        // prev's owner may be CAS-ing its state outside the
        // bucket lock — CAS the .next rewrite. See link_cas_next
        // doc.
        linkage::link_cas_next(
            wait_slot::get_slot(prev_idx).link, static_cast<uint16_t>(next));
      else
        bucket.head = next;
      if (bucket.tail == curr)
        bucket.tail = prev_idx;
      linkage::link_store(slot.link, wait_slot::IDLE,
                             wait_slot::NULL_INDEX);
      slot.wait_address.store(0, cpp::MemoryOrder::RELAXED);
      slot.subsystem.store(wait_slot::SubsystemKind::None,
                           cpp::MemoryOrder::RELAXED);
      curr = next;
      continue;
    }

    bool is_match =
        (st == wait_slot::WAITING || st == wait_slot::IN_KERNEL) &&
        slot.wait_address.load(cpp::MemoryOrder::RELAXED) == target;
    if (!is_match) {
      prev_idx = curr;
      curr = next;
      continue;
    }

    // Bitset filter — slot.wake_bitset was written before the
    // owner's SLL insert under this lock, so bucket.lock acquire
    // gives us a consistent view. No overlap ⇒ leave parked.
    if ((slot.wake_bitset & wake_mask) == 0) {
      prev_idx = curr;
      curr = next;
      continue;
    }

    // [B] Pre-mark CAS WAITING/IN_KERNEL → SIGNALED_CLEAN. CLEAN is
    // correct because the sll_remove below runs under this same
    // lock — atomic from any observer's view. The if-else over `st`
    // adapts the runtime-predicted state to the literal-templated
    // CAS so the SlotState transition table validates at compile
    // time.
    bool claimed;
    uint8_t prev_state;
    if (st == wait_slot::IN_KERNEL) {
      claimed = linkage::link_cas_state<wait_slot::IN_KERNEL,
                                            wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
          slot.link, cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::ACQUIRE);
      prev_state = wait_slot::IN_KERNEL;
      if (!claimed) {
        claimed = linkage::link_cas_state<wait_slot::WAITING,
                                              wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
            slot.link, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::ACQUIRE);
        if (claimed)
          prev_state = wait_slot::WAITING;
      }
    } else {
      claimed = linkage::link_cas_state<wait_slot::WAITING,
                                            wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
          slot.link, cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::ACQUIRE);
      prev_state = wait_slot::WAITING;
      if (!claimed) {
        claimed = linkage::link_cas_state<wait_slot::IN_KERNEL,
                                              wait_slot::SIGNALED_CLEAN, wait_slot::WaitSlotStateTraits>(
            slot.link, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::ACQUIRE);
        if (claimed)
          prev_state = wait_slot::IN_KERNEL;
      }
    }
    if (!claimed) {
      prev_idx = curr;
      curr = next;
      continue;
    }

    // [R] Unlink curr from SLL. State=SIGNALED_CLEAN already set;
    // next is left intact for clear_slot_owned to reset.
    if (prev_idx != wait_slot::NULL_INDEX)
      // prev's owner may be CAS-ing its state outside the bucket
      // lock — CAS the .next rewrite. See link_cas_next doc.
      linkage::link_cas_next(wait_slot::get_slot(prev_idx).link,
                                static_cast<uint16_t>(next));
    else
      bucket.head = next;
    if (bucket.tail == curr)
      bucket.tail = prev_idx;
    // Publish CERT — sll_remove above is the proof of detach. Owner
    // flow goes through clear_slot_owned (which sets CERT anyway),
    // but a thread-exit slot_cleanup racing post-unlink relies on
    // this for its CERT fast-path.
    linkage::link_field_set_cert(slot.link, cpp::MemoryOrder::RELEASE);
    // subsystem AFTER pre-mark so a racing thread-exit trampoline
    // never reads None against a still-WAITING/IN_KERNEL slot.
    slot.subsystem.store(wait_slot::SubsystemKind::None,
                         cpp::MemoryOrder::RELAXED);
    ++woken;

    // I4: capture TID + owner ThreadHandle (packed) BEFORE releasing
    // the lock. Once released, the IN_KERNEL owner can wake (via our
    // alert), exit the wait, and run clear_slot_owned — reading
    // these fields after release would see zeros or a new owner's
    // data after re-alloc.
    uint32_t tid = slot.thread_id.load(cpp::MemoryOrder::RELAXED);
    uint64_t owner_packed = wait_slot::owner_handle_packed(slot);

    // [C] Buffer for batched alert only if IN_KERNEL — WAITING
    // catches via Phase 2.5 cache spin.
    if (prev_state == wait_slot::IN_KERNEL) {
      if (LIBC_LIKELY(scratch_used < scratch_cap)) {
        tgt_buf[scratch_used] = {owner_packed, tid, curr};
        ++scratch_used;
      } else {
        // Overflow (bucket grew by >64 between hint load and lock).
        wait_slot::alert_one_if_live(owner_packed, tid, slot);
      }
    }

    curr = next;
  }

  if (woken > 0)
    bucket.live_count.fetch_sub(static_cast<uint32_t>(woken),
                                cpp::MemoryOrder::RELAXED);
  held.release();

  // Flush outside the lock. count==1 + scratch_used==1 takes the
  // alert_one path (no AutoBoost — avoids over-preemption in
  // handoff chains).
  if (scratch_used > 0) {
    if (count == 1 && scratch_used == 1) {
      wait_slot::alert_one_if_live(tgt_buf[0].owner_packed, tgt_buf[0].tid);
    } else {
      (void)wait_slot::alert_multiple_if_live(tgt_buf, scratch_used,
                                                out_buf, &ab_ctx, 1);
    }
  }
  return woken;
}

template <typename T>
inline long wake(const volatile cpp::Atomic<T> *addr, uint32_t count,
                 uint32_t wake_mask = 0xFFFFFFFFu) {
  return wake(&addr->val, count, wake_mask);
}

// Fork-child reset. The static bucket array survives fork carrying
// parent's head/tail/live_count/lock-flag, but the child's
// wait_slot pool has been rebuilt (every slot on the freelist). A
// stale head/tail would have the first sll_insert/wake traverse
// slots the pool now considers free. Single-threaded at this point,
// no synchronization needed.
LIBC_INLINE void fork_reinit() {
  for (unsigned i = 0; i < BUCKET_COUNT; ++i) {
    Bucket &b = get_bucket(i);
    b.head = wait_slot::NULL_INDEX;
    b.tail = wait_slot::NULL_INDEX;
    b.live_count.store(0, cpp::MemoryOrder::RELAXED);
    b.lock.flag.store(0, cpp::MemoryOrder::RELAXED);
  }
}

} // namespace futex_addr
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_ADDR_H
