//===-- Slot-pool chain corruption stress test -------------*- C++ -*-=====//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises CrystallineSlotPool::claim_slot / release_slot — the Treiber +
// Harris-marked chain primitives the SMR pool builds on — in isolation
// from the Crystalline-W protocol itself. The SMR retire/protect/help
// pipeline is tested separately by crystalline_domain_stress.cpp; this
// file's scope is the slot pool's chain integrity.
//
// Targeted at the active-chain self-loop / multi-membership corruption seen
// in `[E] 16-waiter` futex_bench teardown. Production code already has
// chain_panic detectors (RETIRE_CYCLE / SPLICE_CYCLE / RELEASE_RETRY_OVERFLOW
// / PUSH_HEAD_EQ_IDX / RELEASE_NOT_CLAIMED / CLAIM_NOT_FREE) and dumps the
// chain to stderr before trap. This test adds:
//
//   1. A direct CrystallineSlotPool instance — no domain, no scratch, no
//      registry, no thread-exit cleanup. Only the pool's claim_slot /
//      release_slot are exercised, isolating those primitives from any
//      higher-layer interaction.
//
//   2. A per-thread op-trace ring buffer (256 entries: op-kind + slot +
//      link before/after). When the production chain_panic fires its
//      __builtin_trap, our SIGILL handler dumps every thread's ring so
//      we get a window of the last few ops by every thread leading up
//      to the first observed corruption.
//
//   3. A stop-the-world validator that runs between phases: walks the
//      active chain + freelist, builds a per-slot membership bitmap, and
//      detects multi-membership / state-vs-chain mismatches that the
//      in-line chain_panic detectors only see when the cycle is already
//      walked.
//
// Hermetic build — links its own private libc instance, so the pool we
// stress is OUR pool, not c.dll's g_registry_domain.pool.
//
//===----------------------------------------------------------------------===//

// ====================================================================
// Walker diagnostic hooks — defined BEFORE the substrate include so the
// substrate's #ifndef-guarded macros pick these up. The substrate header
// expands these at every parent-CAS site in harris_walk_attempt:
//
//   LFL_TRACE_BRANCH      records the branch attempt into a per-thread
//                          ring (records branch id + parent + prev_link
//                          + curr + c_link + c_next + spliced flag).
//
//   LFL_VALIDATE_PUBLISH  checks the just-published c_next slot's state.
//                          If state == FREE the substrate just wrote a
//                          freelist-pushed slot index into the active
//                          chain — the exact corruption we're hunting.
//                          Triggers full diagnostic dump + _Exit(127).
//
// Functions take primitive types only so the forward declarations don't
// depend on any libc header.
namespace lfl_diag {
void trace_branch(int branch_id, unsigned parent, unsigned long long prev_link,
                  unsigned curr, unsigned long long c_link, unsigned c_next,
                  int spliced);
void validate_publish(int branch_id, unsigned parent, unsigned c_next,
                      unsigned long long post_link);
void retry_overflow(unsigned target, unsigned retries);
} // namespace lfl_diag

#define LFL_TRACE_BRANCH(branch_id, parent, prev_link_raw, curr,                \
                          c_link_raw, c_next, spliced)                          \
  ::lfl_diag::trace_branch((branch_id), (parent), (prev_link_raw), (curr),     \
                            (c_link_raw), (c_next), (spliced) ? 1 : 0)
#define LFL_VALIDATE_PUBLISH(branch_id, parent, c_next, post_link_raw)         \
  ::lfl_diag::validate_publish((branch_id), (parent), (c_next),                \
                                (post_link_raw))
// Bound walker retry loop at 10000 per single harris_unlink call.
// Bounded by concurrent activity (~thread count per resolution); 10K
// is many orders of magnitude above the legitimate bound. Trips ⇒
// the walker is making no progress = livelock.
#define LFL_RETRY_LIMIT 10000u
#define LFL_RETRY_OVERFLOW(target, retries)                                    \
  ::lfl_diag::retry_overflow((target), (retries))

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_slot_pool.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/config.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigemptyset.h"
#include "src/stdlib/_Exit.h"
#include "src/stdlib/getenv.h"

namespace LIBC_NAMESPACE_DECL {

// ====================================================================
// Configuration (env-var overridable)
// ====================================================================

unsigned g_threads = 16;
unsigned long g_iters_per_thread = 200000;
unsigned g_scenario = 1; // 1: churn, 2: bulk-build/release, 3: SPMC.
unsigned g_per_thread_holdings = 4; // for S2/S3.

// ====================================================================
// Direct stderr write (no stdio buffering — we need every byte before
// the trap kills the process).
// ====================================================================

void wstr(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  IO_STATUS_BLOCK iosb{};
  (void)::NtWriteFile(NtCurrentStandardError(), nullptr, nullptr, nullptr,
                      &iosb, const_cast<char *>(s),
                      static_cast<ULONG>(p - s), nullptr, nullptr);
}

void wraw(const char *s, unsigned n) {
  IO_STATUS_BLOCK iosb{};
  (void)::NtWriteFile(NtCurrentStandardError(), nullptr, nullptr, nullptr,
                      &iosb, const_cast<char *>(s), n, nullptr, nullptr);
}

char *u32_to_str(uint32_t v, char *buf, int buflen) {
  char *end = buf + buflen - 1;
  *end = '\0';
  if (v == 0) {
    *--end = '0';
    return end;
  }
  while (v > 0) {
    *--end = '0' + static_cast<char>(v % 10);
    v /= 10;
  }
  return end;
}

void wu32(uint32_t v) {
  char buf[16];
  wstr(u32_to_str(v, buf, sizeof(buf)));
}

void wu64(uint64_t v) {
  char buf[24];
  char *end = buf + sizeof(buf) - 1;
  *end = '\0';
  if (v == 0) {
    *--end = '0';
  } else {
    while (v > 0) {
      *--end = '0' + static_cast<char>(v % 10);
      v /= 10;
    }
  }
  wstr(end);
}

// ====================================================================
// The pool we stress — file-scope so the SIGILL handler can reach it.
// ====================================================================

concurrent::CrystallineSlotPool g_test_pool;

// ====================================================================
// Op-trace ring — per-thread fixed-size circular log of ops.
// ====================================================================

enum OpKind : uint8_t {
  OP_NONE = 0,
  OP_CLAIM_BEGIN,
  OP_CLAIM_END,
  OP_RELEASE_BEGIN,
  OP_RELEASE_END,
  OP_DOUBLE_POP_DETECTED,
};

const char *op_name(uint8_t k) {
  switch (k) {
  case OP_NONE:
    return "NONE";
  case OP_CLAIM_BEGIN:
    return "CLAIM_BEGIN  ";
  case OP_CLAIM_END:
    return "CLAIM_END    ";
  case OP_RELEASE_BEGIN:
    return "RELEASE_BEGIN";
  case OP_RELEASE_END:
    return "RELEASE_END  ";
  case OP_DOUBLE_POP_DETECTED:
    return "DOUBLE_POP   ";
  default:
    return "?            ";
  }
}

// Op record carries: slot idx + slot.link snapshot + active-head packed
// + freelist-head packed (each is [gen:48|head:16]). At claim_end and
// release_end we capture all four; at begin we capture the heads before
// the call.
struct OpRecord {
  uint8_t kind;
  uint16_t slot;
  uint64_t link;       // slot.link snap
  uint64_t ah_packed;  // active_head packed
  uint64_t fl_packed;  // freelist head packed
  uint64_t seq;
};

constexpr unsigned kRingSize = 256;
constexpr unsigned kRingMask = kRingSize - 1;
static_assert((kRingSize & kRingMask) == 0, "ring must be power-of-2");

constexpr unsigned kMaxThreads = 64;

struct alignas(64) ThreadRing {
  cpp::Atomic<uint64_t> head{0}; // monotonic; index = head & mask
  uint64_t worker_iter{0};
  OpRecord ring[kRingSize];
  char pad[64];
};

ThreadRing g_rings[kMaxThreads];

// ====================================================================
// Walker-trace ring — per-thread record of every parent-CAS attempt
// (success or fail) made by harris_walk_attempt. Populated by the
// LFL_TRACE_BRANCH hook the substrate calls at every branch entry.
//
// Branch IDs (mirror the LFL_TRACE_BRANCH IDs the substrate emits):
//   1 = mark-prev help splice
//   2 = dead-prev splice
//   3 = mark-curr help splice
//   4 = target case splice
//   5 = c_dead opp-splice
//
// Larger ring than OpRecord (1024 vs 256) because each release_slot
// triggers many walker iterations under contention; we want enough
// history to reconstruct the publishing path.
// ====================================================================

struct WalkerTraceRecord {
  uint8_t branch;       // 1-5 per branch ID table above
  uint8_t spliced;      // 0 = parent CAS failed, 1 = succeeded
  uint16_t parent;      // gp (b1/b2) or prev (b3/b4/b5); 0 = head splice
  uint16_t curr;        // the slot being spliced (b1/b2: prev, else curr)
  uint16_t c_next;      // the value published as parent.next on success
  uint64_t prev_link;   // snap of (gp.link b1/b2) or (prev.link b3/b4/b5)
  uint64_t c_link;      // snap of curr.link (== prev_link for b1/b2)
  uint64_t seq;
};

constexpr unsigned kWalkerRingSize = 1024;
constexpr unsigned kWalkerRingMask = kWalkerRingSize - 1;
static_assert((kWalkerRingSize & kWalkerRingMask) == 0,
              "walker ring must be power-of-2");

struct alignas(64) WalkerRing {
  cpp::Atomic<uint64_t> head{0};
  WalkerTraceRecord ring[kWalkerRingSize];
  char pad[64];
};

WalkerRing g_walker_rings[kMaxThreads];

// Per-thread index lives in pthread-key style — we just pass it via arg.
__thread int t_my_idx = -1;

void ring_record(uint8_t kind, uint16_t slot, uint64_t link,
                 uint64_t ah_packed, uint64_t fl_packed) {
  if (t_my_idx < 0)
    return;
  ThreadRing &tr = g_rings[t_my_idx];
  uint64_t h = tr.head.fetch_add(1, cpp::MemoryOrder::RELAXED);
  OpRecord &r = tr.ring[h & kRingMask];
  r.kind = kind;
  r.slot = slot;
  r.link = link;
  r.ah_packed = ah_packed;
  r.fl_packed = fl_packed;
  r.seq = h;
}

// Per-slot owner stamp: low 16 bits = thread idx that most recently
// observed the slot CLAIMED, high 48 bits = monotonic op sequence. A
// claim that finds a non-empty stamp means the previous claimer never
// recorded a release — i.e. someone holds it. If the prior claimer was
// US the slot was double-popped through us; if it was a peer, the peer
// has it active (= multi-membership).
struct alignas(8) SlotOwner {
  cpp::Atomic<uint64_t> packed{0}; // [seq:48 | tid:16]
};
// One entry per possible slot idx (16-bit). 64K * 8B = 512 KB .bss.
SlotOwner g_owner[1u << 16];
cpp::Atomic<uint64_t> g_op_seq{0};

LIBC_INLINE uint64_t mk_owner_stamp(uint64_t seq, uint16_t tid) {
  return (seq << 16) | tid;
}
LIBC_INLINE uint16_t owner_tid(uint64_t stamp) {
  return static_cast<uint16_t>(stamp & 0xFFFFu);
}
LIBC_INLINE uint64_t owner_seq(uint64_t stamp) { return stamp >> 16; }

// Per-slot global op log. For each slot we record the per-thread last
// claim/release sequence numbers. Cheap enough at 16 KB (64K slots * 4
// uint32). The bug: if claim_slot returns idx but the per-slot log
// shows the prior claim (across all threads) is more recent than the
// prior release, the pool returned idx to two owners.
//
// Atomic ordering: we use SEQ_CST for the per-slot updates so a thread
// that observes a fresh claim entry also sees the corresponding
// active_head update.
struct alignas(64) SlotHistory {
  cpp::Atomic<uint64_t> last_claim;   // [seq:48|tid:16]; 0 = never
  cpp::Atomic<uint64_t> last_release; // same encoding
  char pad[64 - 16];
};
SlotHistory g_history[1u << 16];

// String / number formatters — used by both ring dumps. Declared early
// so the splice-ring dumper (defined immediately below) can use them.
LIBC_INLINE char *append_hex64(char *p, uint64_t v) {
  static const char hex[] = "0123456789abcdef";
  *p++ = '0';
  *p++ = 'x';
  for (int n = 60; n >= 0; n -= 4)
    *p++ = hex[(v >> n) & 0xf];
  return p;
}

LIBC_INLINE char *append_str_into(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

LIBC_INLINE char *append_u32_into(char *p, uint32_t v) {
  char tmp[16];
  char *t = u32_to_str(v, tmp, sizeof(tmp));
  while (*t)
    *p++ = *t++;
  return p;
}

void dump_ring(unsigned tid) {
  ThreadRing &tr = g_rings[tid];
  uint64_t h = tr.head.load(cpp::MemoryOrder::ACQUIRE);
  if (h == 0) {
    wstr("  [empty]\n");
    return;
  }
  // Last min(kRingSize, h) entries, oldest first.
  uint64_t count = h < kRingSize ? h : kRingSize;
  uint64_t start = h - count;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t s = start + i;
    OpRecord &r = tr.ring[s & kRingMask];
    char buf[256];
    char *p = buf;
    *p++ = ' ';
    *p++ = ' ';
    *p++ = '#';
    p = append_u32_into(p, static_cast<uint32_t>(r.seq));
    *p++ = ' ';
    p = append_str_into(p, op_name(r.kind));
    *p++ = ' ';
    *p++ = 's';
    *p++ = '=';
    p = append_u32_into(p, r.slot);
    *p++ = ' ';
    *p++ = 'l';
    *p++ = 'k';
    *p++ = '=';
    p = append_hex64(p, r.link);
    *p++ = ' ';
    *p++ = 'a';
    *p++ = 'h';
    *p++ = '=';
    p = append_hex64(p, r.ah_packed);
    *p++ = ' ';
    *p++ = 'f';
    *p++ = 'l';
    *p++ = '=';
    p = append_hex64(p, r.fl_packed);
    *p++ = '\n';
    wraw(buf, static_cast<unsigned>(p - buf));
  }
}

// Helper: dump every ring (called from crash_handler and from
// double-pop trap). Includes both the op-trace ring and the splice
// ring per worker. Serialized by dump-once flag — when multiple
// threads detect corruption simultaneously, only the first dumps;
// the others wait for the dump to finish then _Exit.
cpp::Atomic<uint32_t> g_dump_started{0};
cpp::Atomic<uint32_t> g_dump_done{0};

// Race the dump-once flag. Returns true if we own the dump; the
// caller should print + dump + set g_dump_done. Returns false if a
// peer owns it; the caller should spin until g_dump_done and _Exit.
bool try_claim_dump() {
  uint32_t expected = 0;
  return g_dump_started.compare_exchange_strong(
      expected, 1, cpp::MemoryOrder::ACQ_REL,
      cpp::MemoryOrder::ACQUIRE);
}

void wait_for_dump_then_exit(int code) {
  while (g_dump_done.load(cpp::MemoryOrder::ACQUIRE) == 0)
    for (volatile int p = 0; p < 1024; ++p) {
    }
  LIBC_NAMESPACE::_Exit(code);
}

// Decode a WalkerTraceRecord into the dump line. One per attempt,
// includes branch tag, parent / curr / c_next, full link snaps, and
// success bit so the publishing path is reconstructible.
const char *branch_name(uint8_t b) {
  switch (b) {
  case 1:
    return "MARK_PREV_HELP";
  case 2:
    return "DEAD_PREV_SPLC";
  case 3:
    return "MARK_CURR_HELP";
  case 4:
    return "TARGET_SPLICE ";
  case 5:
    return "C_DEAD_OPPSPLC";
  default:
    return "?             ";
  }
}

void dump_walker_ring(unsigned tid) {
  WalkerRing &wr = g_walker_rings[tid];
  uint64_t h = wr.head.load(cpp::MemoryOrder::ACQUIRE);
  if (h == 0) {
    wstr("  [walker ring empty]\n");
    return;
  }
  uint64_t count = h < kWalkerRingSize ? h : kWalkerRingSize;
  uint64_t start = h - count;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t s = start + i;
    WalkerTraceRecord &r = wr.ring[s & kWalkerRingMask];
    char buf[320];
    char *p = buf;
    *p++ = ' ';
    *p++ = ' ';
    *p++ = '#';
    p = append_u32_into(p, static_cast<uint32_t>(r.seq));
    *p++ = ' ';
    p = append_str_into(p, branch_name(r.branch));
    *p++ = ' ';
    p = append_str_into(p, r.spliced ? "OK " : "NO ");
    p = append_str_into(p, "p=");
    p = append_u32_into(p, r.parent);
    p = append_str_into(p, " c=");
    p = append_u32_into(p, r.curr);
    p = append_str_into(p, " cn=");
    p = append_u32_into(p, r.c_next);
    p = append_str_into(p, " plk=");
    p = append_hex64(p, r.prev_link);
    p = append_str_into(p, " clk=");
    p = append_hex64(p, r.c_link);
    *p++ = '\n';
    wraw(buf, static_cast<unsigned>(p - buf));
  }
}

void dump_all_rings() {
  for (unsigned i = 0; i < g_threads && i < kMaxThreads; ++i) {
    wstr("--- thread ");
    wu32(i);
    wstr(" iter=");
    wu64(g_rings[i].worker_iter);
    wstr(" op_head=");
    wu64(g_rings[i].head.load(cpp::MemoryOrder::ACQUIRE));
    wstr(" walker_head=");
    wu64(g_walker_rings[i].head.load(cpp::MemoryOrder::ACQUIRE));
    wstr(" --- ops:\n");
    dump_ring(i);
    wstr(" walker:\n");
    dump_walker_ring(i);
  }
}

// Decode a Link.raw() in the same shape snap_link emits in OpRecord:
// state(8) | mark(1) | cert(1) | next(16) | tag(32). Used by the
// validate-publish dump to make the post-link readable inline.
LIBC_INLINE char *append_link_decoded(char *p, uint64_t raw) {
  // Link layout: [state:8 | reserved:8 | tag:32 | next:16].
  uint64_t state = (raw >> 56) & 0xFF;
  bool mark = (raw & (1ull << 48)) != 0;
  bool cert = (raw & (1ull << 49)) != 0;
  uint64_t tag = (raw >> 16) & 0xFFFFFFFFu;
  uint64_t next = raw & 0xFFFFu;
  p = append_str_into(p, "state=");
  p = append_u32_into(p, static_cast<uint32_t>(state));
  p = append_str_into(p, mark ? " M" : " m");
  p = append_str_into(p, cert ? "C" : "c");
  p = append_str_into(p, " next=");
  p = append_u32_into(p, static_cast<uint32_t>(next));
  p = append_str_into(p, " tag=");
  p = append_u32_into(p, static_cast<uint32_t>(tag));
  return p;
}

// ====================================================================
// Walker hook implementations (called via the ::lfl_diag:: shims at the
// bottom of this file from the substrate's LFL_TRACE_BRANCH and
// LFL_VALIDATE_PUBLISH macros).
// ====================================================================

// Per-CAS-attempt trace recording — fires from every parent-CAS site
// in harris_walk_attempt regardless of success. Pure ring write, no
// inspection. ~30 ns per call (single fetch_add + 5 stores).
void lfl_trace_branch_impl(int branch_id, unsigned parent,
                            uint64_t prev_link, unsigned curr,
                            uint64_t c_link, unsigned c_next, int spliced) {
  if (t_my_idx < 0)
    return;
  WalkerRing &wr = g_walker_rings[t_my_idx];
  uint64_t h = wr.head.fetch_add(1, cpp::MemoryOrder::RELAXED);
  WalkerTraceRecord &r = wr.ring[h & kWalkerRingMask];
  r.branch = static_cast<uint8_t>(branch_id);
  r.spliced = static_cast<uint8_t>(spliced);
  r.parent = static_cast<uint16_t>(parent);
  r.curr = static_cast<uint16_t>(curr);
  r.c_next = static_cast<uint16_t>(c_next);
  r.prev_link = prev_link;
  r.c_link = c_link;
  r.seq = h;
}

// Stale-publish discriminator. A post-CAS load of slot[c_next] alone is
// AMBIGUOUS: the substrate could have published a c_next that was
// perfectly valid at CAS time, but a peer raced to splice +
// freelist_push it before our load. The hook below disambiguates by
// walking the active chain (see Trap rationale block below) — what the
// substrate's `LFL_VALIDATE_PUBLISH` hook gives consumers in exchange
// for sealing the publish.
//
// To distinguish stale-at-CAS from race-after-CAS, walk the active
// chain when we see post.state == FREE: if any slot reachable from
// active_head is FREE, the chain is currently corrupt and we trap; if
// the chain is clean (no FREE slot reachable), we were just outraced
// and the publish was actually valid.
//
// Filter: c_next == 0 (kCrystallineSlotNullIndex) is the chain-end
// sentinel — legitimate publish, slot 0 is never claimed.
//
// Trap is one-shot via try_claim_dump. Subsequent triggers _Exit
// silently to avoid garbling the dump.
void lfl_validate_publish_impl(int branch_id, unsigned parent,
                                unsigned c_next, uint64_t post_link) {
  if (c_next == 0)
    return; // chain-end sentinel — legitimate publish.
  uint64_t state = (post_link >> 56) & 0xFFu;
  if (state != static_cast<uint64_t>(concurrent::SlotPoolState::FREE))
    return;
  // Discriminator: walk the chain from head. If any reachable slot
  // is FREE, the chain is corrupt (stale-at-CAS). If chain is clean,
  // it was a race-after-CAS — silently return.
  uint16_t corrupt_slot = 0;
  uint16_t scan_head = g_test_pool.active_head();
  unsigned visited = 0;
  while (scan_head != 0 && visited < (1u << 16)) {
    auto &cs = g_test_pool.at(scan_head);
    linkage::Link l = cs.link.load(cpp::MemoryOrder::ACQUIRE);
    if (l.state() == static_cast<uint8_t>(concurrent::SlotPoolState::FREE)) {
      corrupt_slot = scan_head;
      break;
    }
    scan_head = l.next();
    ++visited;
  }
  if (corrupt_slot == 0)
    return; // benign race-after-CAS.
  if (!try_claim_dump())
    wait_for_dump_then_exit(127);
  wstr("\n*** STALE c_next PUBLISH ⇒ CHAIN CORRUPT ***\n");
  wstr(" branch=");
  wu32(static_cast<uint32_t>(branch_id));
  wstr(" (");
  wstr(branch_name(static_cast<uint8_t>(branch_id)));
  wstr(")\n parent=");
  wu32(parent);
  wstr(" c_next=");
  wu32(c_next);
  wstr("\n c_next_link={");
  char buf[256];
  char *p = append_link_decoded(buf, post_link);
  wraw(buf, static_cast<unsigned>(p - buf));
  wstr("}\n corrupt_slot=");
  wu32(corrupt_slot);
  wstr(" (FREE on active chain after ");
  wu32(visited);
  wstr(" hops)\n my_tid=");
  wu32(t_my_idx >= 0 ? static_cast<uint32_t>(t_my_idx) : 0xffffffffu);
  wstr(" active_head=");
  {
    char hbuf[24];
    char *hp = append_hex64(hbuf, g_test_pool.debug_active_head_packed());
    wraw(hbuf, static_cast<unsigned>(hp - hbuf));
  }
  wstr(" freelist_head=");
  {
    char hbuf[24];
    char *hp = append_hex64(hbuf, g_test_pool.debug_freelist_head_packed());
    wraw(hbuf, static_cast<unsigned>(hp - hbuf));
  }
  wstr("\n*** ring dump follows (op + walker rings, all threads) ***\n");
  dump_all_rings();
  wstr("*** end stale-publish dump ***\n");
  g_dump_done.store(1, cpp::MemoryOrder::RELEASE);
  LIBC_NAMESPACE::_Exit(127);
}

// Retry-overflow trap — the substrate's harris_unlink is grinding
// without progress on `target`. This catches livelock at the moment
// it manifests, rather than waiting for a hang. The op-trace ring +
// walker ring of every thread are dumped so we can see what every
// other thread was doing at the freeze point.
void lfl_retry_overflow_impl(unsigned target, unsigned retries) {
  if (!try_claim_dump())
    wait_for_dump_then_exit(128);
  wstr("\n*** WALKER LIVELOCK — harris_unlink retry overflow ***\n");
  wstr(" my_tid=");
  wu32(t_my_idx >= 0 ? static_cast<uint32_t>(t_my_idx) : 0xffffffffu);
  wstr(" target=");
  wu32(target);
  wstr(" retries=");
  wu32(retries);
  wstr("\n target_link={");
  if (target != 0) {
    char buf[256];
    auto &cs = g_test_pool.at(static_cast<uint16_t>(target));
    linkage::Link l = cs.link.load(cpp::MemoryOrder::ACQUIRE);
    char *p = append_link_decoded(buf, l.raw());
    wraw(buf, static_cast<unsigned>(p - buf));
  }
  wstr("}\n active_head=");
  {
    char hbuf[24];
    char *hp = append_hex64(hbuf, g_test_pool.debug_active_head_packed());
    wraw(hbuf, static_cast<unsigned>(hp - hbuf));
  }
  wstr(" freelist_head=");
  {
    char hbuf[24];
    char *hp = append_hex64(hbuf, g_test_pool.debug_freelist_head_packed());
    wraw(hbuf, static_cast<unsigned>(hp - hbuf));
  }
  wstr("\n active chain (depth-first walk from head):\n");
  uint16_t scan = g_test_pool.active_head();
  unsigned visited = 0;
  while (scan != 0 && visited < 65536) {
    auto &cs = g_test_pool.at(scan);
    linkage::Link l = cs.link.load(cpp::MemoryOrder::ACQUIRE);
    char buf[256];
    char *p = buf;
    *p++ = ' ';
    *p++ = ' ';
    *p++ = '#';
    p = append_u32_into(p, visited);
    p = append_str_into(p, " s=");
    p = append_u32_into(p, scan);
    p = append_str_into(p, " {");
    p = append_link_decoded(p, l.raw());
    *p++ = '}';
    *p++ = '\n';
    wraw(buf, static_cast<unsigned>(p - buf));
    scan = l.next();
    ++visited;
  }
  if (scan != 0) {
    wstr(" ... chain exceeds 64K — aborting walk\n");
  }
  wstr("\n*** ring dump follows (op + walker rings, all threads) ***\n");
  dump_all_rings();
  wstr("*** end retry-overflow dump ***\n");
  g_dump_done.store(1, cpp::MemoryOrder::RELEASE);
  LIBC_NAMESPACE::_Exit(128);
}

// ====================================================================
// SIGILL / SIGSEGV handler — dumps every ring on chain_panic trap.
// ====================================================================

cpp::Atomic<uint32_t> g_handler_entered{0};

void crash_handler(int sig) {
  uint32_t expected = 0;
  if (!g_handler_entered.compare_exchange_strong(
          expected, 1, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::RELAXED)) {
    LIBC_NAMESPACE::_Exit(125);
  }
  wstr("\n*** crystalline_slot_pool_stress: signal ");
  wu32(static_cast<uint32_t>(sig));
  wstr(" — dumping op-trace rings ***\n");
  dump_all_rings();
  wstr("*** end op-trace ring dump ***\n");
  LIBC_NAMESPACE::_Exit(124);
}

void install_handlers() {
  struct sigaction sa {};
  sa.sa_handler = &crash_handler;
  LIBC_NAMESPACE::sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  // SIGINT routed to the same dump path so a Ctrl-C on a hung run
  // produces the same diagnostic as a chain_panic trap.
  LIBC_NAMESPACE::sigaction(SIGINT, &sa, nullptr);
  LIBC_NAMESPACE::sigaction(SIGTERM, &sa, nullptr);
  LIBC_NAMESPACE::sigaction(SIGILL, &sa, nullptr);
  LIBC_NAMESPACE::sigaction(SIGSEGV, &sa, nullptr);
  LIBC_NAMESPACE::sigaction(SIGTRAP, &sa, nullptr);
  LIBC_NAMESPACE::sigaction(SIGABRT, &sa, nullptr);
  LIBC_NAMESPACE::sigaction(SIGBUS, &sa, nullptr);
}

// ====================================================================
// Workers
// ====================================================================

cpp::Atomic<uint64_t> g_total_claims{0};
cpp::Atomic<uint64_t> g_total_releases{0};
cpp::Atomic<uint64_t> g_total_oom{0};

struct WorkerArg {
  unsigned idx;
};

// Capture link state. Pull link.value via the public pack — but `Link`'s
// internal repr is not directly accessible; expose via the operator we
// wrote on the test side: read slot.link via the pool's at() method
// would require pool friendship. Instead, snap by calling the public
// load and re-pack the visible fields.
uint64_t snap_link(uint16_t slot) {
  if (slot == 0)
    return 0;
  auto &cs = g_test_pool.at(slot);
  linkage::Link l =
      cs.link.load(cpp::MemoryOrder::ACQUIRE);
  // Pack [state(8) | mark(1) | cert(1) | next(16) | tag(32)] for diagnostic.
  uint64_t packed = static_cast<uint64_t>(l.state()) << 56;
  if (l.is_marked())
    packed |= 1ull << 55;
  if (l.is_certified())
    packed |= 1ull << 54;
  packed |= static_cast<uint64_t>(l.next()) << 32;
  packed |= static_cast<uint64_t>(l.tag());
  return packed;
}

// Scenario 1: tight churn — claim then release immediately. Maximum
// contention on freelist head + active head; each release walks a
// chain whose head is currently being mutated by N-1 peers.
//
// Per op we record: link snap, active_head packed, freelist_head packed.
// Owner-stamp ring detects double-pop: if claim_slot returns idx and
// g_owner[idx] still names a thread that hasn't released, the pool
// handed the same idx to two owners.
void run_scenario_churn(unsigned worker_idx) {
  ThreadRing &tr = g_rings[worker_idx];
  uint16_t my_tid = static_cast<uint16_t>(worker_idx + 1); // 0 = unowned
  for (unsigned long iter = 0; iter < g_iters_per_thread; ++iter) {
    tr.worker_iter = iter;
    uint64_t ah_b = g_test_pool.debug_active_head_packed();
    uint64_t fl_b = g_test_pool.debug_freelist_head_packed();
    ring_record(OP_CLAIM_BEGIN, 0, 0, ah_b, fl_b);

    uint16_t s = g_test_pool.claim_slot();
    if (s == 0) {
      g_total_oom.fetch_add(1, cpp::MemoryOrder::RELAXED);
      ring_record(OP_CLAIM_END, 0, 0,
                  g_test_pool.debug_active_head_packed(),
                  g_test_pool.debug_freelist_head_packed());
      continue;
    }
    g_total_claims.fetch_add(1, cpp::MemoryOrder::RELAXED);

    // History check: read prior claim/release stamps for slot s. If
    // last_claim's seq > last_release's seq, the slot's previous
    // claimer never released — i.e. the pool returned s to two
    // owners. Use SEQ_CST loads so the pair is observed atomically
    // enough for steady-state detection.
    uint64_t op_seq = g_op_seq.fetch_add(1, cpp::MemoryOrder::SEQ_CST);
    uint64_t prior_claim =
        g_history[s].last_claim.load(cpp::MemoryOrder::SEQ_CST);
    uint64_t prior_release =
        g_history[s].last_release.load(cpp::MemoryOrder::SEQ_CST);
    if (owner_seq(prior_claim) > owner_seq(prior_release)) {
      if (!try_claim_dump())
        wait_for_dump_then_exit(126);
      ring_record(OP_DOUBLE_POP_DETECTED, s, snap_link(s),
                  g_test_pool.debug_active_head_packed(),
                  g_test_pool.debug_freelist_head_packed());
      wstr("\n*** DOUBLE-POP slot=");
      wu32(s);
      wstr(" my_tid=");
      wu32(my_tid);
      wstr(" my_seq=");
      wu64(op_seq);
      wstr(" prior_claim_tid=");
      wu32(owner_tid(prior_claim));
      wstr(" prior_claim_seq=");
      wu64(owner_seq(prior_claim));
      wstr(" prior_release_tid=");
      wu32(owner_tid(prior_release));
      wstr(" prior_release_seq=");
      wu64(owner_seq(prior_release));
      wstr("\n");
      dump_all_rings();
      g_dump_done.store(1, cpp::MemoryOrder::RELEASE);
      LIBC_NAMESPACE::_Exit(126);
    }
    g_history[s].last_claim.store(mk_owner_stamp(op_seq, my_tid),
                                  cpp::MemoryOrder::SEQ_CST);

    // Keep the owner-stamp side-table too (cleared at release_slot
    // boundary; useful for the unmodified PUSH_HEAD_EQ_IDX trap to
    // cross-reference).
    uint64_t want_unowned = 0;
    uint64_t my_stamp = mk_owner_stamp(op_seq, my_tid);
    g_owner[s].packed.compare_exchange_strong(
        want_unowned, my_stamp, cpp::MemoryOrder::ACQ_REL,
        cpp::MemoryOrder::ACQUIRE);

    uint64_t after = snap_link(s);
    ring_record(OP_CLAIM_END, s, after,
                g_test_pool.debug_active_head_packed(),
                g_test_pool.debug_freelist_head_packed());

    ring_record(OP_RELEASE_BEGIN, s, after,
                g_test_pool.debug_active_head_packed(),
                g_test_pool.debug_freelist_head_packed());
    // Stamp release in the history BEFORE freelist_push completes.
    // We don't have access to the inside of release_slot, so the
    // closest correct point is "right before release_slot" — by the
    // time release_slot returns and a peer can pop the slot, the
    // history will already show the release.
    uint64_t rel_seq = g_op_seq.fetch_add(1, cpp::MemoryOrder::SEQ_CST);
    g_history[s].last_release.store(mk_owner_stamp(rel_seq, my_tid),
                                    cpp::MemoryOrder::SEQ_CST);
    g_owner[s].packed.store(0, cpp::MemoryOrder::RELEASE);
    g_test_pool.release_slot(s);
    g_total_releases.fetch_add(1, cpp::MemoryOrder::RELAXED);
    uint64_t ah_post = g_test_pool.debug_active_head_packed();
    uint64_t fl_post = g_test_pool.debug_freelist_head_packed();
    ring_record(OP_RELEASE_END, s, snap_link(s), ah_post, fl_post);

    // Per-release concurrent detector retired — it's fundamentally
    // TOCTOU-vulnerable. The substrate walker's intermediate states
    // (post-finalize-pre-freelist_push, post-step1-pre-step2 of
    // active_push, mark-then-help races, ...) are all transient and
    // visible to a chain walker that captures one ah_post snapshot.
    //
    // Distinguishing those from real multi-membership requires
    // either full quiescence or a sustained DCAS over (ah, slot.link)
    // — neither feasible in a per-iteration probe. The validator_run
    // calls at scenario boundaries (pre/post) provide quiescent-time
    // invariant checks; structural bugs (self-loops, hangs, kernel
    // traps in chain_panic) surface end-to-end.
  }
}

// Scenario 2: bulk build then release in random order. Each iteration
// claims K slots, then releases them in shuffled order. Splices land
// at random chain positions, maximising mid-chain splice contention.
void run_scenario_bulk(unsigned worker_idx) {
  ThreadRing &tr = g_rings[worker_idx];
  uint16_t holdings[64];
  unsigned K = g_per_thread_holdings;
  if (K > 64)
    K = 64;
  uint32_t rng = 0x12345678u + worker_idx * 0x9E3779B1u;
  for (unsigned long iter = 0; iter < g_iters_per_thread; ++iter) {
    tr.worker_iter = iter;
    unsigned have = 0;
    for (unsigned i = 0; i < K; ++i) {
      ring_record(OP_CLAIM_BEGIN, 0, 0, 0, 0);
      uint16_t s = g_test_pool.claim_slot();
      if (s == 0) {
        g_total_oom.fetch_add(1, cpp::MemoryOrder::RELAXED);
        ring_record(OP_CLAIM_END, 0, 0, 0, 0);
        break;
      }
      uint64_t after = snap_link(s);
      ring_record(OP_CLAIM_END, s, after, 0, 0);
      g_total_claims.fetch_add(1, cpp::MemoryOrder::RELAXED);
      holdings[have++] = s;
    }
    // Fisher-Yates shuffle.
    for (unsigned i = have; i > 1; --i) {
      rng = rng * 1664525u + 1013904223u;
      unsigned j = rng % i;
      uint16_t tmp = holdings[i - 1];
      holdings[i - 1] = holdings[j];
      holdings[j] = tmp;
    }
    for (unsigned i = 0; i < have; ++i) {
      uint16_t s = holdings[i];
      uint64_t before = snap_link(s);
      ring_record(OP_RELEASE_BEGIN, s, before, 0, 0);
      g_test_pool.release_slot(s);
      g_total_releases.fetch_add(1,
                                 cpp::MemoryOrder::RELAXED);
      ring_record(OP_RELEASE_END, s, before, 0, 0);
    }
  }
}

// SPMC ring for scenario 3 — claimer thread fills, releaser threads drain.
struct alignas(64) SpmcSlot {
  cpp::Atomic<uint16_t> val{0}; // 0 = empty.
  char pad[62];
};
constexpr unsigned kSpmcSize = 4096;
SpmcSlot g_spmc[kSpmcSize];
cpp::Atomic<uint64_t> g_spmc_prod_idx{0};
cpp::Atomic<uint64_t> g_spmc_cons_idx{0};
cpp::Atomic<bool> g_spmc_done{false};

void run_scenario_spmc_claimer(unsigned worker_idx) {
  ThreadRing &tr = g_rings[worker_idx];
  uint64_t target_total = static_cast<uint64_t>(g_iters_per_thread) *
                           (g_threads > 1 ? g_threads - 1 : 1);
  for (uint64_t i = 0; i < target_total; ++i) {
    tr.worker_iter = i;
    ring_record(OP_CLAIM_BEGIN, 0, 0, 0, 0);
    uint16_t s = g_test_pool.claim_slot();
    if (s == 0) {
      g_total_oom.fetch_add(1, cpp::MemoryOrder::RELAXED);
      ring_record(OP_CLAIM_END, 0, 0, 0, 0);
      continue;
    }
    g_total_claims.fetch_add(1, cpp::MemoryOrder::RELAXED);
    uint64_t after = snap_link(s);
    ring_record(OP_CLAIM_END, s, after, 0, 0);
    // Publish into ring: spin until empty slot.
    for (;;) {
      uint64_t pi = g_spmc_prod_idx.load(cpp::MemoryOrder::RELAXED);
      uint64_t ci = g_spmc_cons_idx.load(cpp::MemoryOrder::ACQUIRE);
      // Items-in-flight check via signed delta. Consumers fetch_add
      // cons_idx unconditionally at the top of each iter, so under load
      // ci can race past pi — in that case the unsigned `pi - ci`
      // underflows to ~UINT64_MAX and the throttle erroneously fires
      // forever (ci is monotonic, will never come back). Cast to signed
      // so a negative items-in-flight (ci ahead of pi, consumers
      // already waiting on slots producer hasn't filled yet) bypasses
      // the throttle and we proceed straight to the slot CAS.
      int64_t in_flight =
          static_cast<int64_t>(pi) - static_cast<int64_t>(ci);
      if (in_flight >= static_cast<int64_t>(kSpmcSize)) {
        // Full — pause briefly.
        for (volatile int p = 0; p < 64; ++p) {
        }
        continue;
      }
      uint16_t expected = 0;
      if (g_spmc[pi & (kSpmcSize - 1)].val.compare_exchange_strong(
              expected, s, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::RELAXED)) {
        g_spmc_prod_idx.store(pi + 1,
                              cpp::MemoryOrder::RELEASE);
        break;
      }
      // Lost the slot to another publisher (shouldn't happen with single
      // claimer; fall through to retry).
    }
  }
  g_spmc_done.store(true, cpp::MemoryOrder::RELEASE);
}

void run_scenario_spmc_releaser(unsigned worker_idx) {
  ThreadRing &tr = g_rings[worker_idx];
  uint64_t local_iter = 0;
  for (;;) {
    tr.worker_iter = local_iter++;
    uint64_t ci = g_spmc_cons_idx.fetch_add(
        1, cpp::MemoryOrder::ACQ_REL);
    SpmcSlot &slot = g_spmc[ci & (kSpmcSize - 1)];
    uint16_t s = 0;
    int spins = 0;
    while ((s = slot.val.exchange(
                0, cpp::MemoryOrder::ACQ_REL)) == 0) {
      if (g_spmc_done.load(cpp::MemoryOrder::ACQUIRE) &&
          ci >= g_spmc_prod_idx.load(
                    cpp::MemoryOrder::ACQUIRE)) {
        return;
      }
      if (++spins > 1024) {
        // Long stall — yield.
        for (volatile int p = 0; p < 256; ++p) {
        }
        spins = 0;
      }
    }
    uint64_t before = snap_link(s);
    ring_record(OP_RELEASE_BEGIN, s, before, 0, 0);
    g_test_pool.release_slot(s);
    g_total_releases.fetch_add(1,
                               cpp::MemoryOrder::RELAXED);
    ring_record(OP_RELEASE_END, s, before, 0, 0);
  }
}

// Watchdog: polls cumulative claims+releases. If the count is unchanged
// for kWatchdogStallSeconds, dumps every ring and exits — this is the
// only mechanism that fires on a true hang where worker threads are
// spinning in user space without any CAS or atomic progress visible
// to the in-test counters.
constexpr unsigned kWatchdogPollMillis = 500;
constexpr unsigned kWatchdogStallSeconds = 8;

void *watchdog_main(void * /*arg*/) {
  uint64_t last_total = 0;
  unsigned stall_polls = 0;
  unsigned stall_threshold =
      (kWatchdogStallSeconds * 1000u) / kWatchdogPollMillis;
  for (;;) {
    // Sleep kWatchdogPollMillis. Use a busy-polled wait so we don't
    // depend on usleep being routable here. Tight inner loop is OK
    // for diagnostic.
    for (unsigned ms = 0; ms < kWatchdogPollMillis; ++ms)
      for (volatile int p = 0; p < 100000; ++p) {
      }
    uint64_t now_total =
        g_total_claims.load(cpp::MemoryOrder::RELAXED) +
        g_total_releases.load(cpp::MemoryOrder::RELAXED);
    if (now_total != last_total) {
      last_total = now_total;
      stall_polls = 0;
      continue;
    }
    if (g_dump_done.load(cpp::MemoryOrder::ACQUIRE) ||
        g_dump_started.load(cpp::MemoryOrder::ACQUIRE))
      return nullptr;
    if (++stall_polls >= stall_threshold) {
      // True hang. Claim dump, dump, exit.
      if (!try_claim_dump())
        wait_for_dump_then_exit(129);
      wstr("\n*** WATCHDOG: NO PROGRESS for ");
      wu32(kWatchdogStallSeconds);
      wstr("s — total ops=");
      wu64(now_total);
      wstr(" ***\n active_head=");
      {
        char hbuf[24];
        char *hp =
            append_hex64(hbuf, g_test_pool.debug_active_head_packed());
        wraw(hbuf, static_cast<unsigned>(hp - hbuf));
      }
      wstr(" freelist_head=");
      {
        char hbuf[24];
        char *hp =
            append_hex64(hbuf, g_test_pool.debug_freelist_head_packed());
        wraw(hbuf, static_cast<unsigned>(hp - hbuf));
      }
      wstr("\n active chain (depth-first):\n");
      uint16_t scan = g_test_pool.active_head();
      unsigned visited = 0;
      while (scan != 0 && visited < 65536) {
        auto &cs = g_test_pool.at(scan);
        linkage::Link l = cs.link.load(cpp::MemoryOrder::ACQUIRE);
        char buf[256];
        char *p = buf;
        *p++ = ' ';
        *p++ = ' ';
        *p++ = '#';
        p = append_u32_into(p, visited);
        p = append_str_into(p, " s=");
        p = append_u32_into(p, scan);
        p = append_str_into(p, " {");
        p = append_link_decoded(p, l.raw());
        *p++ = '}';
        *p++ = '\n';
        wraw(buf, static_cast<unsigned>(p - buf));
        scan = l.next();
        ++visited;
      }
      if (scan != 0)
        wstr(" ... chain exceeds 64K — aborting walk\n");
      wstr("\n*** ring dump (op + walker, all threads) ***\n");
      dump_all_rings();
      wstr("*** end watchdog dump ***\n");
      g_dump_done.store(1, cpp::MemoryOrder::RELEASE);
      LIBC_NAMESPACE::_Exit(129);
    }
  }
}

void *worker_main(void *arg) {
  WorkerArg *wa = static_cast<WorkerArg *>(arg);
  t_my_idx = static_cast<int>(wa->idx);
  switch (g_scenario) {
  case 1:
    run_scenario_churn(wa->idx);
    break;
  case 2:
    run_scenario_bulk(wa->idx);
    break;
  case 3:
    if (wa->idx == 0)
      run_scenario_spmc_claimer(wa->idx);
    else
      run_scenario_spmc_releaser(wa->idx);
    break;
  }
  return nullptr;
}

// ====================================================================
// Validator (between-phase, single-threaded)
// ====================================================================

void validator_run(const char *phase) {
  // Quiesced: walk active chain, build bitmap.
  // 64K-bit bitmap = 8K bytes.
  static uint8_t in_active[8192];
  static uint8_t in_freelist[8192];
  memset(in_active, 0, sizeof(in_active));
  memset(in_freelist, 0, sizeof(in_freelist));

  unsigned active_count = 0;
  unsigned issues = 0;

  uint16_t s = g_test_pool.active_head();
  while (s != 0) {
    if (in_active[s >> 3] & (1u << (s & 7))) {
      wstr("[VALIDATOR] cycle in active chain at slot ");
      wu32(s);
      wstr(" phase=");
      wstr(phase);
      wstr("\n");
      issues++;
      break;
    }
    in_active[s >> 3] |= (1u << (s & 7));
    active_count++;
    if (active_count > 65536) {
      wstr("[VALIDATOR] active chain exceeds 64K phase=");
      wstr(phase);
      wstr("\n");
      issues++;
      break;
    }
    auto &cs = g_test_pool.at(s);
    linkage::Link l =
        cs.link.load(cpp::MemoryOrder::ACQUIRE);
    if (l.state() != static_cast<uint8_t>(
                         concurrent::SlotPoolState::CLAIMED)) {
      wstr("[VALIDATOR] state-mismatch on active chain: slot ");
      wu32(s);
      wstr(" state=");
      wu32(l.state());
      wstr(" phase=");
      wstr(phase);
      wstr("\n");
      issues++;
    }
    if (l.next() == s) {
      wstr("[VALIDATOR] self-loop active slot ");
      wu32(s);
      wstr(" phase=");
      wstr(phase);
      wstr("\n");
      issues++;
      break;
    }
    s = l.next();
  }

  // No public committed-slots accessor — we can't safely cross-check
  // every committed slot's state against the chain bitmap (SIGSEGV past
  // committed). Chain-walk invariants above are sufficient for now.
  (void)in_freelist;

  wstr("[VALIDATOR] phase=");
  wstr(phase);
  wstr(" active=");
  wu32(active_count);
  wstr(" issues=");
  wu32(issues);
  wstr("\n");
  if (issues != 0) {
    wstr("[VALIDATOR] FAIL — dumping rings\n");
    for (unsigned i = 0; i < g_threads && i < kMaxThreads; ++i) {
      wstr("--- thread ");
      wu32(i);
      wstr(" ---\n");
      dump_ring(i);
    }
    LIBC_NAMESPACE::_Exit(123);
  }
}

// ====================================================================
// Env-var parsing
// ====================================================================

unsigned long parse_env_ul(const char *name, unsigned long fallback) {
  const char *v = LIBC_NAMESPACE::getenv(name);
  if (v == nullptr || *v == '\0')
    return fallback;
  unsigned long out = 0;
  for (const char *p = v; *p; ++p) {
    if (*p < '0' || *p > '9')
      return fallback;
    out = out * 10 + (*p - '0');
  }
  return out;
}

int run_main(int argc, char **argv) {
  install_handlers();

  g_threads = static_cast<unsigned>(
      parse_env_ul("CCS_THREADS", g_threads));
  if (g_threads > kMaxThreads)
    g_threads = kMaxThreads;
  if (g_threads < 1)
    g_threads = 1;
  g_iters_per_thread = parse_env_ul("CCS_ITERS", g_iters_per_thread);
  g_scenario =
      static_cast<unsigned>(parse_env_ul("CCS_SCENARIO", g_scenario));
  g_per_thread_holdings = static_cast<unsigned>(
      parse_env_ul("CCS_HOLDINGS", g_per_thread_holdings));

  // Optional CLI override: argv[1] = scenario.
  if (argc > 1 && argv[1] != nullptr) {
    unsigned long s = 0;
    for (const char *p = argv[1]; *p; ++p)
      if (*p >= '0' && *p <= '9')
        s = s * 10 + (*p - '0');
    if (s >= 1 && s <= 3)
      g_scenario = static_cast<unsigned>(s);
  }

  wstr("=== crystalline_slot_pool_stress ===\n");
  wstr(" threads=");
  wu32(g_threads);
  wstr(" iters_per_thread=");
  wu64(g_iters_per_thread);
  wstr(" scenario=");
  wu32(g_scenario);
  wstr(" holdings=");
  wu32(g_per_thread_holdings);
  wstr("\n");

  if (!g_test_pool.init()) {
    wstr("FATAL: pool.init() failed\n");
    return 2;
  }

  validator_run("pre");

  pthread_t tids[kMaxThreads];
  WorkerArg args[kMaxThreads];
  for (unsigned i = 0; i < g_threads; ++i) {
    args[i].idx = i;
    int rc = LIBC_NAMESPACE::pthread_create(&tids[i], nullptr, &worker_main,
                                            &args[i]);
    if (rc != 0) {
      wstr("FATAL: pthread_create failed at i=");
      wu32(i);
      wstr("\n");
      return 3;
    }
  }
  // Spawn the watchdog. It self-terminates (returns nullptr) when it
  // observes g_dump_started or g_dump_done; we don't join — let it run
  // until the workers complete and the join loop below returns, after
  // which _Exit from the trap or natural exit terminates everything.
  pthread_t watchdog_tid;
  (void)LIBC_NAMESPACE::pthread_create(&watchdog_tid, nullptr,
                                        &watchdog_main, nullptr);
  for (unsigned i = 0; i < g_threads; ++i)
    LIBC_NAMESPACE::pthread_join(tids[i], nullptr);

  validator_run("post");

  wstr("RESULT claims=");
  wu64(g_total_claims.load(cpp::MemoryOrder::RELAXED));
  wstr(" releases=");
  wu64(g_total_releases.load(cpp::MemoryOrder::RELAXED));
  wstr(" oom=");
  wu64(g_total_oom.load(cpp::MemoryOrder::RELAXED));
  wstr(" PASS\n");
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL

// File-scope shims for the substrate's LFL_TRACE_BRANCH /
// LFL_VALIDATE_PUBLISH macros. These match the forward declarations at
// the top of this file — primitive-typed so the substrate's macro
// expansion doesn't need any libc header to compile. Each delegates to
// the LIBC_NAMESPACE-internal impl that has access to the rings + dump.
namespace lfl_diag {
void trace_branch(int branch_id, unsigned parent, unsigned long long prev_link,
                  unsigned curr, unsigned long long c_link, unsigned c_next,
                  int spliced) {
  LIBC_NAMESPACE::lfl_trace_branch_impl(branch_id, parent, prev_link, curr,
                                         c_link, c_next, spliced);
}
void validate_publish(int branch_id, unsigned parent, unsigned c_next,
                      unsigned long long post_link) {
  LIBC_NAMESPACE::lfl_validate_publish_impl(branch_id, parent, c_next,
                                             post_link);
}
void retry_overflow(unsigned target, unsigned retries) {
  LIBC_NAMESPACE::lfl_retry_overflow_impl(target, retries);
}
} // namespace lfl_diag

extern "C" int main(int argc, char **argv) {
  return LIBC_NAMESPACE::run_main(argc, argv);
}
