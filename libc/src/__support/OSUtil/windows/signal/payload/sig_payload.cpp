//===-- Rich signal payload subsystem -- implementation ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/payload/sig_payload.h"

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace payload {

namespace {

// Discriminated payload record. Inherits the SMR bookkeeping from
// CrystallineNode; the runtime owns those fields across retire/reclaim.
//
// Union sized to the largest variant (SIGCHLD: 24 B). Discriminator +
// signum + si_code + union ≈ 36 B, plus CrystallineNode's ~32 B for
// total ~68 B per record.
struct SigPayload : ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // [0..19] Intrusive Crystalline-W runtime fields emitted via the
  // shared macro so SigPayload remains standard-layout.
  LIBC_CRYSTALLINE_NODE_FIELDS(SigPayload);

  enum Kind : unsigned char {
    SIGCHLD_PAYLOAD,
    TIMER_PAYLOAD,
    CROSS_KILL_PAYLOAD,
  };
  Kind kind;
  int signum;
  int si_code;
  union {
    struct {
      int pid;
      int status;
      long long utime_us;
      long long stime_us;
    } sigchld;
    struct {
      int timerid;
      int overrun;
      union sigval value;
    } timer;
    struct {
      int sender_pid;
      unsigned sender_uid;
      union sigval value;
    } xkill;
  };
};

// Pool size — comfortable overprovision over (max concurrent writers ≈ 4
// drain threads + 1 timer thread + ALPC listener) × (in-flight retires
// before reclaim). 64 records × ~68 B ≈ 4.4 KB. Pool exhaustion drops
// only the rich payload for that event; the pend bit still wakes the
// receiver and POSIX permits the fields to be best-effort.
constexpr uint32_t kPoolSize = 64;

// Crystalline retire frequency. 4 publishes-promptly (matches art_index's
// kArtRetireFreq); SIGCHLD is a low-rate signal so larger batching adds
// memory hold without amortization benefit.
constexpr uint32_t kRetireFreq = 4;

SigPayload g_pool[kPoolSize] = {};

// Treiber-stack freelist. Head is a 64-bit word: high 16 bits = ABA
// generation counter, low 32 bits = (record index + 1) of the top
// (0 = empty). The gen monotonically increases per push, so a CAS that
// observes the same head value implies no intervening pop+push cycle.
//
// Per-record next-link lives in g_freelist_links to keep SigPayload
// itself purely Crystalline-owned (the runtime's union expects no
// trailing user state interleaved with its bookkeeping fields).
cpp::Atomic<uint64_t> g_freelist_head{0};
cpp::Atomic<uint32_t> g_freelist_links[kPoolSize] = {};

constexpr uint64_t kHeadGenShift = 48;
constexpr uint64_t kHeadIdxMask = (1ull << 32) - 1;

LIBC_INLINE uint32_t head_idx_plus_one(uint64_t head) {
  return static_cast<uint32_t>(head & kHeadIdxMask);
}

LIBC_INLINE uint64_t head_pack(uint64_t old_head, uint32_t new_idx_plus_one) {
  uint64_t gen = (old_head >> kHeadGenShift) + 1;
  return (gen << kHeadGenShift) | static_cast<uint64_t>(new_idx_plus_one);
}

// FreeFn for Crystalline. Called once per record when no reader can still
// reach it. Returns the slot to the pool freelist for reuse.
void return_to_pool(SigPayload *r) {
  if (LIBC_UNLIKELY(r == nullptr))
    __builtin_trap();
  uint32_t idx = static_cast<uint32_t>(r - g_pool);
  if (LIBC_UNLIKELY(idx >= kPoolSize))
    __builtin_trap();

  uint64_t cur = g_freelist_head.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    g_freelist_links[idx].store(head_idx_plus_one(cur),
                                cpp::MemoryOrder::RELAXED);
    uint64_t next = head_pack(cur, idx + 1);
    if (g_freelist_head.compare_exchange_weak(cur, next,
                                              cpp::MemoryOrder::ACQ_REL,
                                              cpp::MemoryOrder::ACQUIRE))
      return;
  }
}

// Pop from the freelist. Returns nullptr on exhaustion. Wait-free in
// the no-contention case; bounded retries under contention (CAS is a
// finite operation per attempt and the head's gen counter forces every
// concurrent pop or push to make a real change before our CAS retries).
SigPayload *alloc_from_pool() {
  uint64_t cur = g_freelist_head.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    uint32_t top = head_idx_plus_one(cur);
    if (top == 0)
      return nullptr;
    uint32_t idx = top - 1;
    uint32_t next_top =
        g_freelist_links[idx].load(cpp::MemoryOrder::RELAXED);
    uint64_t next = head_pack(cur, next_top);
    if (g_freelist_head.compare_exchange_weak(cur, next,
                                              cpp::MemoryOrder::ACQ_REL,
                                              cpp::MemoryOrder::ACQUIRE))
      return &g_pool[idx];
  }
}

} // namespace

// BatchLinkCodec specialization for SigPayload — must precede the
// CrystallineDomain instantiation below so the substrate's encode/decode
// calls resolve. SigPayload sits in a fixed BSS pool of kPoolSize=64
// entries; index `(s - g_pool)` is the natural identity, shifted by 1
// to keep 0 as the unretired sentinel.
} // namespace payload
} // namespace signal_state
namespace concurrent {
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::signal_state::payload::SigPayload> {
  using Node = ::LIBC_NAMESPACE::signal_state::payload::SigPayload;
  LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {
    auto *s = static_cast<Node *>(n);
    return 1u + static_cast<uint32_t>(
                    s - ::LIBC_NAMESPACE::signal_state::payload::g_pool);
  }
  LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {
    return &::LIBC_NAMESPACE::signal_state::payload::g_pool[code - 1u];
  }
};
} // namespace concurrent
namespace signal_state {
namespace payload {
namespace {

// Single Crystalline domain instance. One of 8 available domains
// (see crystalline_slot_pool.h).
::LIBC_NAMESPACE::concurrent::CrystallineDomain<SigPayload, &return_to_pool,
                                                kRetireFreq>
    g_domain;

// Per-signum "latest event" pointer. POSIX-correct: standard signals
// coalesce, only the latest event's payload survives — that is literally
// what each entry stores. Indexed by (signum - 1).
cpp::Atomic<SigPayload *> g_latest[NSIG] = {};

// Common publish path: exchange the prepared record into the per-signum
// latest slot, retire the previous record (if any) for asynchronous
// reclaim. Caller has already alloc'd, init_node'd, and filled fields.
LIBC_INLINE void publish_common(int signum, SigPayload *prepared) {
  SigPayload *prev = g_latest[signum - 1].exchange(prepared,
                                                    cpp::MemoryOrder::RELEASE);
  if (prev)
    g_domain.retire(prev);
}

LIBC_INLINE bool valid_signum(int signum) {
  return signum >= 1 && signum < NSIG;
}

} // namespace

void init_payload_subsystem() {
  // Build the freelist by pushing every pool slot. Single-threaded init,
  // so the CAS loop in return_to_pool is uncontended.
  for (uint32_t i = 0; i < kPoolSize; ++i)
    return_to_pool(&g_pool[i]);
  g_domain.init_registration();
}

void fini_payload_subsystem() {
  for (int i = 0; i < NSIG; ++i)
    g_latest[i].store(nullptr, cpp::MemoryOrder::RELEASE);
}

void fork_reinit_payload_subsystem() {
  // Child starts with no SIGCHLD/timer/cross-kill state from the parent.
  // The pool itself is copied COW with all records as the parent left
  // them; clearing the latest pointers means no reader can reach them
  // and the parent's published records become reusable on the next
  // alloc. The Crystalline domain's own fork hook (registered via
  // init_registration) handles per-thread retire-batch reset.
  for (int i = 0; i < NSIG; ++i)
    g_latest[i].store(nullptr, cpp::MemoryOrder::RELAXED);
}

void publish_sigchld(int code, int pid, int status, long long utime_us,
                     long long stime_us) {
  SigPayload *r = alloc_from_pool();
  if (!r)
    return;
  g_domain.init_node(r);
  r->kind = SigPayload::SIGCHLD_PAYLOAD;
  r->signum = SIGCHLD;
  r->si_code = code;
  r->sigchld.pid = pid;
  r->sigchld.status = status;
  r->sigchld.utime_us = utime_us;
  r->sigchld.stime_us = stime_us;
  publish_common(SIGCHLD, r);
}

void publish_timer_signal(int signum, int timerid, int overrun,
                          union sigval value) {
  if (!valid_signum(signum))
    return;
  SigPayload *r = alloc_from_pool();
  if (!r)
    return;
  g_domain.init_node(r);
  r->kind = SigPayload::TIMER_PAYLOAD;
  r->signum = signum;
  r->si_code = SI_TIMER;
  r->timer.timerid = timerid;
  r->timer.overrun = overrun;
  r->timer.value = value;
  publish_common(signum, r);
}

void publish_cross_kill(int signum, int code, int sender_pid,
                        unsigned sender_uid, union sigval value) {
  if (!valid_signum(signum))
    return;
  SigPayload *r = alloc_from_pool();
  if (!r)
    return;
  g_domain.init_node(r);
  r->kind = SigPayload::CROSS_KILL_PAYLOAD;
  r->signum = signum;
  r->si_code = code;
  r->xkill.sender_pid = sender_pid;
  r->xkill.sender_uid = sender_uid;
  r->xkill.value = value;
  publish_common(signum, r);
}

void populate_signal_payload(siginfo_t *info, int signum) {
  if (!valid_signum(signum))
    return;

  // Wait-free protected dereference. The record is guaranteed valid for
  // the duration of our reservation; clear_all() below releases it.
  SigPayload *r = g_domain.protect(g_latest[signum - 1], /*index=*/0,
                                    /*parent=*/nullptr);
  if (!r)
    return;

  info->si_signo = r->signum;
  info->si_code = r->si_code;
  switch (r->kind) {
  case SigPayload::SIGCHLD_PAYLOAD:
    info->si_pid = r->sigchld.pid;
    info->si_status = r->sigchld.status;
    info->si_utime = static_cast<clock_t>(r->sigchld.utime_us);
    info->si_stime = static_cast<clock_t>(r->sigchld.stime_us);
    break;
  case SigPayload::TIMER_PAYLOAD:
    info->si_timerid = r->timer.timerid;
    info->si_overrun = r->timer.overrun;
    info->si_value = r->timer.value;
    break;
  case SigPayload::CROSS_KILL_PAYLOAD:
    info->si_pid = r->xkill.sender_pid;
    info->si_uid = r->xkill.sender_uid;
    info->si_value = r->xkill.value;
    break;
  }

  g_domain.clear_all();
}

} // namespace payload
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
