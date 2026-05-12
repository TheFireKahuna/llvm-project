//===-- Crystalline-W domain SMR protocol stress test ------*- C++ -*-=====//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises CrystallineDomain<NodeT, FreeFn>::{init_node, protect, retire,
// clear_all, warm_thread} as the wait-free SMR protocol that Layer 1's
// va_tracker::resolve() depends on. The slot-pool chain primitives the
// domain composes are tested separately by crystalline_slot_pool_stress.cpp;
// this file's scope is the SMR contract itself — pinned objects must
// not be freed while a reservation is held; reclamation must converge.
//
// Witness model:
//   - TestNode carries cpp::Atomic<bool> freed and a monotonic
//     cpp::Atomic<uint64_t> published_tag. The FreeFn flips freed and
//     records the freeing tag; readers verify the post-protect snapshot
//     of published_tag against a held pin and assert freed == false.
//   - No test-only is_pinned(slot_idx) query is consulted (none exists);
//     the contract under test is end-to-end SMR safety, which the
//     freed/published_tag pair already witnesses without breaking the
//     per-pointer pin discipline by exposing slot internals.
//
// Threads use real NtCreateThreadEx via pthread_create; the start gate
// is an atomic spin matching crystalline_slot_pool_stress.cpp (a Futex
// gate would buy nothing for a one-shot startup signal and would pull
// the full futex_utils translation unit into the hermetic link).
//
// Scenarios (CDS_SCENARIO env var):
//   1 — ProtectRetireLifetime          (single reader/retirer)
//   2 — MultiThreadedProtectRetire     (4R + 1 retirer + 1 publisher)
//   3 — TwoSlotRotation                (ART-style ping-pong; slot A pin
//                                       survives slot B rotation)
//   4 — ClearAllDoesNotLeakRetired     (1K retires + clear_all + drain
//                                       drives free_fn count to 1K)
//   5 — SixteenDomainBudget            (kMaxCrystallineDomains=16
//                                       registrations succeed with
//                                       unique IDs in [0, 15])
//   6 — SlowPathReclaim                (DEFERRED — see notes)
//   7 — AdversarialReclaimRace         (8 readers + 8 retirers, 1M ops;
//                                       ABA via published_tag mismatch)
//
//===----------------------------------------------------------------------===//

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_local_state.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/config.h"
#include "src/pthread/pthread_create.h"
#include "src/pthread/pthread_join.h"
#include "src/stdlib/_Exit.h"
#include "src/stdlib/getenv.h"

namespace LIBC_NAMESPACE_DECL {

// ====================================================================
// Configuration (env-var overridable)
// ====================================================================

unsigned long g_iters_per_thread = 100000;
unsigned g_scenario = 1;
unsigned long g_s7_iters = 1000000;
unsigned long g_s4_retires = 1000;
unsigned long g_s4_drain_budget = 50000;

// ====================================================================
// Direct stderr writers — bypass stdio so a failing assertion shows up
// even if the trap kills the process before the runtime flushes.
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

// Failure protocol: print a line + trap. The trap is preferred over
// _Exit because the post-mortem stack trace localizes the failing
// scenario without needing per-scenario exit codes.
[[noreturn]] void fail(const char *tag, uint64_t observed, uint64_t expected) {
  wstr("\n*** crystalline_domain_stress FAIL: ");
  wstr(tag);
  wstr(" observed=");
  wu64(observed);
  wstr(" expected=");
  wu64(expected);
  wstr(" ***\n");
  __builtin_trap();
}

// ====================================================================
// TestNode + BatchLinkCodec
// ====================================================================
//
// The 16-domain budget scenario needs 16 distinct CrystallineDomain<>
// template instantiations, which in turn needs 16 distinct NodeT
// types. TestNodeN<I> generates them; the primary I=0 specialization
// is used by every other scenario.
//
// `slot_idx` is the codec field — encode = 1 + slot_idx, decode looks
// up `g_test_node_pool<I>[code - 1]`. The +1 reserves 0 as the codec's
// unretired sentinel (matches the in-tree BatchLinkCodec contract).

constexpr uint32_t kPoolCap = 4096;

template <uint32_t I> struct TestNodeT : public concurrent::CrystallineNode {
  LIBC_CRYSTALLINE_NODE_FIELDS(TestNodeT<I>);
  // User fields below the Crystalline substrate fields.
  cpp::Atomic<bool> freed;
  cpp::Atomic<uint64_t> published_tag;
  uint32_t pool_index; // matches BatchLinkCodec encode/decode below
};

using TestNode = TestNodeT<0>;

// Pool storage — one per template instantiation. The Crystalline FreeFn
// runs on memory we own here; no malloc/free interaction.
template <uint32_t I> struct PoolStorage {
  static TestNodeT<I> arr[kPoolCap];
};
template <uint32_t I> TestNodeT<I> PoolStorage<I>::arr[kPoolCap]{};

template <uint32_t I>
TestNodeT<I> *pool_at(uint32_t idx) {
  return &PoolStorage<I>::arr[idx];
}

// Reclamation witnesses — flipped by FreeFn, observed by main thread.
template <uint32_t I> struct ReclaimWitness {
  static cpp::Atomic<uint64_t> free_fn_count;
  static cpp::Atomic<uint64_t> last_freed_tag;
};
template <uint32_t I>
cpp::Atomic<uint64_t> ReclaimWitness<I>::free_fn_count{0};
template <uint32_t I>
cpp::Atomic<uint64_t> ReclaimWitness<I>::last_freed_tag{0};

// FreeFn — invoked exactly once per node when the Crystalline-W proof
// guarantees no reader holds a reservation that could observe `node`
// via a published pointer. UAF detector: if `node->freed` is already
// true, the protocol reclaimed the same node twice; if a concurrent
// reader observes `node->freed == true` while holding a pin against
// `node->published_tag`, the protocol freed under a held reservation.
template <uint32_t I> void test_free_fn(TestNodeT<I> *node) {
  bool was_freed = node->freed.exchange(true, cpp::MemoryOrder::ACQ_REL);
  if (was_freed)
    fail("DOUBLE_FREE", node->pool_index, 0);
  ReclaimWitness<I>::free_fn_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  ReclaimWitness<I>::last_freed_tag.store(
      node->published_tag.load(cpp::MemoryOrder::ACQUIRE),
      cpp::MemoryOrder::RELEASE);
}

} // namespace LIBC_NAMESPACE_DECL

// BatchLinkCodec specializations live in ::LIBC_NAMESPACE::concurrent per
// the template's lookup rule (declared inside that namespace in
// crystalline_domain.h). The specialization indexes into the per-I
// pool storage.

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

template <uint32_t I>
struct BatchLinkCodecForTest {
  using Node = ::LIBC_NAMESPACE::TestNodeT<I>;
  LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {
    return 1u + static_cast<Node *>(n)->pool_index;
  }
  LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {
    return ::LIBC_NAMESPACE::pool_at<I>(code - 1u);
  }
};

#define LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(I)                                  \
  template <> struct BatchLinkCodec<::LIBC_NAMESPACE::TestNodeT<I>> {          \
    using Node = ::LIBC_NAMESPACE::TestNodeT<I>;                               \
    LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {          \
      return BatchLinkCodecForTest<I>::encode(n);                              \
    }                                                                          \
    LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {       \
      return BatchLinkCodecForTest<I>::decode(code);                           \
    }                                                                          \
  }

LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(0);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(1);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(2);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(3);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(4);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(5);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(6);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(7);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(8);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(9);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(10);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(11);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(12);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(13);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(14);
LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC(15);

#undef LIBC_CRYSTALLINE_DOMAIN_TEST_CODEC

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {

// ====================================================================
// Domain instances — one per template instantiation. Construction is
// trivial; init_registration() runs from main() pre-thread-spawn.
// ====================================================================

template <uint32_t I> struct DomainHolder {
  static concurrent::CrystallineDomain<TestNodeT<I>, &test_free_fn<I>> dom;
};
template <uint32_t I>
concurrent::CrystallineDomain<TestNodeT<I>, &test_free_fn<I>>
    DomainHolder<I>::dom;

template <uint32_t I> auto &dom_of() { return DomainHolder<I>::dom; }

// ====================================================================
// Start gate + pool reset
// ====================================================================

cpp::Atomic<int> g_start{0};
cpp::Atomic<unsigned> g_running{0};

void wait_for_start() {
  while (g_start.load(cpp::MemoryOrder::ACQUIRE) == 0) {
    // Brief CPU pause; one-shot startup gate.
    __asm__ __volatile__("pause" ::: "memory");
  }
}

template <uint32_t I> void reset_pool_state() {
  for (uint32_t i = 0; i < kPoolCap; ++i) {
    auto *n = pool_at<I>(i);
    n->freed.store(false, cpp::MemoryOrder::RELEASE);
    n->published_tag.store(0, cpp::MemoryOrder::RELEASE);
    n->pool_index = i;
    n->batch_link.store(0u, cpp::MemoryOrder::RELEASE);
  }
  ReclaimWitness<I>::free_fn_count.store(0, cpp::MemoryOrder::RELEASE);
  ReclaimWitness<I>::last_freed_tag.store(0, cpp::MemoryOrder::RELEASE);
}

// ====================================================================
// Scenario 1: ProtectRetireLifetime
// ====================================================================
//
// Single thread: publish node A; protect-read A pinning slot 0 with
// the current era; retire A; under the held pin, A->freed must remain
// false. Then publish a replacement and re-protect on slot 0 — this
// advances the slot's era[0] reservation past A's birth_era, releasing
// the pin. Drive retires to amortize try_retire — A->freed becomes
// true. Note: clear_all alone does not advance era[index]; only a
// subsequent protect on the same slot does. The era-rotation pattern
// matches the production ART consumer.

cpp::Atomic<TestNode *> g_s1_slot{nullptr};

void run_scenario_1() {
  wstr("scenario 1: ProtectRetireLifetime\n");
  reset_pool_state<0>();
  auto &dom = dom_of<0>();
  dom.warm_thread();

  // Publish a node.
  TestNode *a = pool_at<0>(0);
  dom.init_node(a);
  a->published_tag.store(0xA11CE, cpp::MemoryOrder::RELEASE);
  g_s1_slot.store(a, cpp::MemoryOrder::RELEASE);

  // Pin via protect — writes a's era into era[0].
  TestNode *pinned = dom.protect(g_s1_slot, 0, nullptr);
  if (pinned != a)
    fail("S1_protect_mismatch", reinterpret_cast<uint64_t>(pinned),
         reinterpret_cast<uint64_t>(a));

  // Retire while pinned.
  dom.retire(a);

  // Drive retires to fire try_retire and bump era_ several times. While
  // era[0] still pins a's birth_era, a must remain unfreed.
  for (uint32_t i = 1; i < 1024; ++i) {
    TestNode *n = pool_at<0>(i);
    dom.init_node(n);
    n->published_tag.store(0xB000ULL + i, cpp::MemoryOrder::RELEASE);
    dom.retire(n);
  }

  if (a->freed.load(cpp::MemoryOrder::ACQUIRE))
    fail("S1_freed_while_pinned", 1, 0);

  // Publish a replacement on slot 0 and re-protect. The protect's fast
  // path observes era drift and runs do_update, advancing era[0] past
  // a's birth_era — slot 0 no longer pins a.
  TestNode *cycle = pool_at<0>(1024);
  dom.init_node(cycle);
  cycle->published_tag.store(0xC1C1E, cpp::MemoryOrder::RELEASE);
  g_s1_slot.store(cycle, cpp::MemoryOrder::RELEASE);
  TestNode *recycled = dom.protect(g_s1_slot, 0, nullptr);
  if (recycled != cycle)
    fail("S1_recycle_protect_mismatch",
         reinterpret_cast<uint64_t>(recycled),
         reinterpret_cast<uint64_t>(cycle));

  // Drive more retires; no slot pins era ≤ a.birth_era anymore, so
  // try_retire reclaims a within the bounded drive.
  for (uint32_t i = 1025; i < kPoolCap; ++i) {
    TestNode *n = pool_at<0>(i);
    dom.init_node(n);
    n->published_tag.store(0xD000ULL + i, cpp::MemoryOrder::RELEASE);
    dom.retire(n);
  }
  dom.clear_all();

  if (!a->freed.load(cpp::MemoryOrder::ACQUIRE))
    fail("S1_not_freed_after_unpin", 0, 1);

  wstr("  pass\n");
}

// ====================================================================
// Scenario 2: MultiThreadedProtectRetire
// ====================================================================
//
// 4 readers + 1 retirer + 1 publisher, all on domain<0>. Publisher
// rotates a fresh node into g_s2_slot, retiring the previous; readers
// pin via protect and assert the pinned node is not freed.

constexpr unsigned kS2Readers = 4;
constexpr uint32_t kS2RingSize = 256;

cpp::Atomic<TestNode *> g_s2_slot{nullptr};
cpp::Atomic<uint64_t> g_s2_publish_tag{1};
cpp::Atomic<unsigned> g_s2_publisher_done{0};

struct S2WorkerArg {
  unsigned idx;
};

S2WorkerArg g_s2_args[kS2Readers + 2];

void *s2_reader(void *raw) {
  auto *wa = static_cast<S2WorkerArg *>(raw);
  auto &dom = dom_of<0>();
  dom.warm_thread();
  wait_for_start();
  (void)wa;

  for (unsigned long it = 0; it < g_iters_per_thread; ++it) {
    TestNode *p = dom.protect(g_s2_slot, 0, nullptr);
    if (p != nullptr) {
      // Pin held — p must not be observed freed.
      if (p->freed.load(cpp::MemoryOrder::ACQUIRE))
        fail("S2_freed_while_pinned", it, 0);
      // Touch published_tag to force the load to materialize against
      // the pinned region; defeats DCE.
      uint64_t tag = p->published_tag.load(cpp::MemoryOrder::ACQUIRE);
      if (tag == 0xDEADBEEFDEADBEEFULL)
        fail("S2_torn_tag", tag, 0);
    }
  }
  return nullptr;
}

void *s2_publisher(void *) {
  auto &dom = dom_of<0>();
  dom.warm_thread();
  wait_for_start();

  // Cycle through pool slots, publishing-then-retiring.
  uint32_t idx = 0;
  for (unsigned long it = 0; it < g_iters_per_thread; ++it) {
    TestNode *n = pool_at<0>(idx);
    // Reuse slot only after prior occupant has been freed — otherwise
    // we'd init_node over a still-pinned address. With kPoolCap=4096
    // and 1 publisher this is comfortable for the test's iter count;
    // assert defensively for paranoia.
    if (!n->freed.load(cpp::MemoryOrder::ACQUIRE) && it >= kPoolCap)
      fail("S2_reuse_before_free", idx, it);
    n->freed.store(false, cpp::MemoryOrder::RELEASE);
    dom.init_node(n);
    uint64_t tag = g_s2_publish_tag.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
    n->published_tag.store(tag, cpp::MemoryOrder::RELEASE);

    TestNode *prev =
        g_s2_slot.exchange(n, cpp::MemoryOrder::ACQ_REL);
    if (prev != nullptr)
      dom.retire(prev);

    idx = (idx + 1) % kPoolCap;
  }
  g_s2_publisher_done.store(1, cpp::MemoryOrder::RELEASE);
  return nullptr;
}

void run_scenario_2() {
  wstr("scenario 2: MultiThreadedProtectRetire\n");
  reset_pool_state<0>();
  g_s2_slot.store(nullptr, cpp::MemoryOrder::RELEASE);
  g_s2_publish_tag.store(1, cpp::MemoryOrder::RELEASE);
  g_s2_publisher_done.store(0, cpp::MemoryOrder::RELEASE);

  pthread_t readers[kS2Readers];
  pthread_t publisher;
  for (unsigned i = 0; i < kS2Readers; ++i) {
    g_s2_args[i].idx = i;
    LIBC_NAMESPACE::pthread_create(&readers[i], nullptr, &s2_reader,
                                    &g_s2_args[i]);
  }
  LIBC_NAMESPACE::pthread_create(&publisher, nullptr, &s2_publisher, nullptr);

  g_start.store(1, cpp::MemoryOrder::RELEASE);

  for (unsigned i = 0; i < kS2Readers; ++i)
    LIBC_NAMESPACE::pthread_join(readers[i], nullptr);
  LIBC_NAMESPACE::pthread_join(publisher, nullptr);

  g_start.store(0, cpp::MemoryOrder::RELEASE);
  wstr("  pass\n");
}

// ====================================================================
// Scenario 3: TwoSlotRotation
// ====================================================================
//
// ART descent pattern: a "parent" pin on slot 0 must remain valid
// across a "child" pin on slot 1 (which may rotate slot 1 but not
// slot 0). The two slots map onto Crystalline-W indices 0 and 1,
// which on the ART consumer side carry the kArtPinSlotDescendA/B
// names; this test exercises the substrate guarantee directly.

cpp::Atomic<TestNode *> g_s3_slot_a{nullptr};
cpp::Atomic<TestNode *> g_s3_slot_b{nullptr};

void run_scenario_3() {
  wstr("scenario 3: TwoSlotRotation\n");
  reset_pool_state<0>();
  auto &dom = dom_of<0>();
  dom.warm_thread();

  // Publish A on slot 0, B on slot 1.
  TestNode *a = pool_at<0>(0);
  TestNode *b0 = pool_at<0>(1);
  dom.init_node(a);
  dom.init_node(b0);
  a->published_tag.store(0xA, cpp::MemoryOrder::RELEASE);
  b0->published_tag.store(0xB, cpp::MemoryOrder::RELEASE);
  g_s3_slot_a.store(a, cpp::MemoryOrder::RELEASE);
  g_s3_slot_b.store(b0, cpp::MemoryOrder::RELEASE);

  // Pin A on index 0 and B on index 1.
  TestNode *pa = dom.protect(g_s3_slot_a, 0, nullptr);
  TestNode *pb = dom.protect(g_s3_slot_b, 1, nullptr);
  if (pa != a || pb != b0)
    fail("S3_initial_protect_mismatch", reinterpret_cast<uint64_t>(pa),
         reinterpret_cast<uint64_t>(a));

  // Rotate slot 1 to B1, retire B0. Slot 0 (carrying A) untouched.
  TestNode *b1 = pool_at<0>(2);
  dom.init_node(b1);
  b1->published_tag.store(0xB1, cpp::MemoryOrder::RELEASE);
  g_s3_slot_b.store(b1, cpp::MemoryOrder::RELEASE);
  dom.retire(b0);

  // Re-protect slot 1 — drops the B0 pin, picks up B1. Slot 0's pin
  // on A is untouched.
  TestNode *pb1 = dom.protect(g_s3_slot_b, 1, nullptr);
  if (pb1 != b1)
    fail("S3_slot1_rotate_mismatch", reinterpret_cast<uint64_t>(pb1),
         reinterpret_cast<uint64_t>(b1));

  // Retire A. Slot 0 still pins it.
  dom.retire(a);

  // Drive amortization with unrelated retires.
  for (uint32_t i = 3; i < 256; ++i) {
    TestNode *n = pool_at<0>(i);
    dom.init_node(n);
    n->published_tag.store(0x3000 + i, cpp::MemoryOrder::RELEASE);
    dom.retire(n);
  }

  // Slot-0 pin on A must hold across the slot-1 rotation + amortization.
  if (a->freed.load(cpp::MemoryOrder::ACQUIRE))
    fail("S3_slot0_pin_broken_by_slot1_rotation", 1, 0);

  // Re-protect slot 0 to drop A; then verify A drains. The replacement
  // is allocated past the 256 nodes the amortization loop above used.
  TestNode *a_replace = pool_at<0>(256);
  dom.init_node(a_replace);
  a_replace->published_tag.store(0xAA, cpp::MemoryOrder::RELEASE);
  g_s3_slot_a.store(a_replace, cpp::MemoryOrder::RELEASE);
  TestNode *pa2 = dom.protect(g_s3_slot_a, 0, nullptr);
  if (pa2 != a_replace)
    fail("S3_slot0_drop_mismatch", reinterpret_cast<uint64_t>(pa2),
         reinterpret_cast<uint64_t>(a_replace));

  dom.clear_all();
  for (uint32_t i = 257; i < kPoolCap; ++i) {
    TestNode *n = pool_at<0>(i);
    dom.init_node(n);
    n->published_tag.store(0x3500 + i, cpp::MemoryOrder::RELEASE);
    dom.retire(n);
  }
  dom.clear_all();

  if (!a->freed.load(cpp::MemoryOrder::ACQUIRE))
    fail("S3_slot0_drained_did_not_reclaim", 0, 1);

  wstr("  pass\n");
}

// ====================================================================
// Scenario 4: ClearAllDoesNotLeakRetired
// ====================================================================
//
// Drive g_s4_retires retires from a single thread, call clear_all,
// then drive a follow-up flush burst. Eventually every retired node's
// FreeFn must run — free_fn_count converges to g_s4_retires within a
// bounded drive budget.

void run_scenario_4() {
  wstr("scenario 4: ClearAllDoesNotLeakRetired\n");
  reset_pool_state<0>();
  auto &dom = dom_of<0>();
  dom.warm_thread();

  if (g_s4_retires > kPoolCap)
    fail("S4_retires_exceeds_pool", g_s4_retires, kPoolCap);

  for (uint32_t i = 0; i < g_s4_retires; ++i) {
    TestNode *n = pool_at<0>(i);
    dom.init_node(n);
    n->published_tag.store(0x4000 + i, cpp::MemoryOrder::RELEASE);
    dom.retire(n);
  }
  // clear_all is the API-boundary drop; the test never called protect,
  // so era[*] remains at the slot's init sentinel (0) and no slot pins
  // any retiree. Reclamation comes purely from try_retire amortization
  // as the follow-up retires fire it.
  dom.clear_all();

  // Drive follow-up retires (reusing freed pool slots once the FreeFn
  // marks them) until every original retiree is reclaimed or the
  // budget runs out.
  uint64_t drained =
      ReclaimWitness<0>::free_fn_count.load(cpp::MemoryOrder::ACQUIRE);
  uint32_t cursor = static_cast<uint32_t>(g_s4_retires);
  for (uint32_t loop = 0; loop < g_s4_drain_budget; ++loop) {
    TestNode *n = nullptr;
    for (uint32_t step = 0; step < kPoolCap; ++step) {
      TestNode *candidate = pool_at<0>(cursor);
      cursor = (cursor + 1) % kPoolCap;
      if (cursor == 0)
        cursor = static_cast<uint32_t>(g_s4_retires); // skip original retires
      if (candidate->freed.exchange(false, cpp::MemoryOrder::ACQ_REL)) {
        n = candidate;
        break;
      }
      // Slot beyond the original retires that was never retired yet —
      // batch_link == 0 marks it unretired. Safe to init_node + retire.
      if (candidate->batch_link.load(cpp::MemoryOrder::RELAXED) == 0u) {
        n = candidate;
        break;
      }
    }
    if (n == nullptr)
      break; // no reusable slot — degenerate (would indicate stuck reclaim)

    dom.init_node(n);
    n->published_tag.store(0x5000ULL + loop, cpp::MemoryOrder::RELEASE);
    dom.retire(n);

    drained =
        ReclaimWitness<0>::free_fn_count.load(cpp::MemoryOrder::ACQUIRE);
    if (drained >= g_s4_retires)
      break;
  }
  dom.clear_all();

  if (drained < g_s4_retires)
    fail("S4_did_not_drain", drained, g_s4_retires);

  wstr("  pass\n");
}

// ====================================================================
// Scenario 5: SixteenDomainBudget — DEFERRED
// ====================================================================
//
// The roadmap's scenario asks for 16 fresh CrystallineDomain<>
// registrations to succeed and the 17th to trap. The trap side cannot
// be exercised in-process without subprocess machinery (registry_push
// uses __builtin_trap on overflow). The success side is also unsafe
// to run in this hermetic linkage: libc's static init has already
// registered several production domains (arena, va_tracker × 5,
// partition — per the live cross-cutting tracking item), consuming an
// unknown fraction of the kMaxCrystallineDomains=16 budget. The exact
// count is not exposed at compile time, so a fixed test-side
// registration count would either trap (if production count grows) or
// fail to cover the full budget (if production count shrinks).
//
// Land scenario 5 once a subprocess-isolated death-test framework is
// available for the trap path, or once `registry_domains_registered()`
// is exposed so the test can register exactly the remaining budget.

void run_scenario_5() {
  wstr("scenario 5: SixteenDomainBudget DEFERRED (production "
       "co-registration uncertainty)\n");
  wstr("  skipped (re-enable after subprocess death-test or "
       "registry-count query lands)\n");
}

// ====================================================================
// Scenario 6: SlowPathReclaim — DEFERRED
// ====================================================================
//
// The slow_path / help_thread split that this scenario wants to
// observe directly is pending the `detach_nodes` extraction from
// `slow_path` + `help_thread`. Without that split there is no
// observable boundary between "spilled retires drained via slow_path"
// and "drained via help_thread" — the two share the same per-slot
// try_retire body and the test would not add coverage beyond what
// scenario 7's adversarial race already exercises end-to-end.
//
// Land scenario 6 in the same PR that extracts `detach_nodes`.

void run_scenario_6() {
  wstr("scenario 6: SlowPathReclaim DEFERRED (detach_nodes pending)\n");
  wstr("  skipped (re-enable after extraction lands)\n");
}

// ====================================================================
// Scenario 7: AdversarialReclaimRace
// ====================================================================
//
// 8 readers + 8 retirers, 1M ops total. Readers protect a node and
// validate its tag against a captured published_tag; retirers cycle
// fresh nodes into a shared slot, retiring the previous occupant.
//
// ABA witness: after protect, the reader snapshots published_tag.
// If the pin is broken (UAF or torn read), a freshly republished
// node at the same address will carry a different (larger)
// published_tag — the reader detects this and fails.

constexpr unsigned kS7Readers = 8;
constexpr unsigned kS7Retirers = 8;
cpp::Atomic<TestNode *> g_s7_slot{nullptr};
cpp::Atomic<uint64_t> g_s7_publish_tag{1};

struct S7WorkerArg {
  unsigned idx;
};
S7WorkerArg g_s7_args[kS7Readers + kS7Retirers];

void *s7_reader(void *raw) {
  auto *wa = static_cast<S7WorkerArg *>(raw);
  auto &dom = dom_of<0>();
  dom.warm_thread();
  wait_for_start();
  (void)wa;

  unsigned long iters = g_s7_iters / kS7Readers;
  for (unsigned long it = 0; it < iters; ++it) {
    TestNode *p = dom.protect(g_s7_slot, 0, nullptr);
    if (p == nullptr)
      continue;
    // Snapshot the tag under the pin.
    uint64_t tag_before = p->published_tag.load(cpp::MemoryOrder::ACQUIRE);
    if (tag_before == 0)
      continue; // raced with init_node-not-yet-tagged; benign

    // Force a memory dependency on `p` — defeats compiler elimination.
    asm volatile("" : : "r"(p) : "memory");

    // Re-read the tag. Under the pin the node cannot have been freed
    // and reused, so the tag must be unchanged from our snapshot.
    uint64_t tag_after = p->published_tag.load(cpp::MemoryOrder::ACQUIRE);
    if (tag_after != tag_before)
      fail("S7_aba_tag_changed_under_pin", tag_after, tag_before);

    // Verify pin: if freed under us, we have UAF.
    if (p->freed.load(cpp::MemoryOrder::ACQUIRE))
      fail("S7_uaf_freed_under_pin", tag_before, 0);
  }
  return nullptr;
}

void *s7_retirer(void *raw) {
  auto *wa = static_cast<S7WorkerArg *>(raw);
  auto &dom = dom_of<0>();
  dom.warm_thread();
  wait_for_start();

  // Per-retirer pool partition eliminates the cross-retirer TOCTOU
  // window in the slot-reuse claim — within a partition only this
  // thread writes the `freed` flag, so a plain exchange is sound.
  constexpr unsigned long partition_size = kPoolCap / kS7Retirers;
  const unsigned long base = static_cast<unsigned long>(wa->idx) * partition_size;
  unsigned long local_cursor = 0;

  unsigned long iters = g_s7_iters / kS7Retirers;
  unsigned long skipped = 0;
  for (unsigned long it = 0; it < iters; ++it) {
    // Find a reusable slot in this thread's partition: either FreeFn
    // has run (freed==true) or the slot has never been published.
    uint32_t idx = kPoolCap;
    for (unsigned step = 0; step < partition_size; ++step) {
      uint32_t candidate =
          static_cast<uint32_t>(base + local_cursor);
      local_cursor = (local_cursor + 1) % partition_size;
      TestNode *n = pool_at<0>(candidate);
      if (n->freed.exchange(false, cpp::MemoryOrder::ACQ_REL)) {
        idx = candidate;
        break;
      }
      if (n->published_tag.load(cpp::MemoryOrder::ACQUIRE) == 0) {
        idx = candidate;
        break;
      }
    }
    if (idx == kPoolCap) {
      // Partition saturated with retired-but-not-yet-freed slots.
      // Yield by skipping this iteration; the retire pump from peer
      // threads will drive amortization forward.
      ++skipped;
      continue;
    }
    TestNode *n = pool_at<0>(idx);
    dom.init_node(n);
    uint64_t tag = g_s7_publish_tag.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
    n->published_tag.store(tag, cpp::MemoryOrder::RELEASE);

    TestNode *prev = g_s7_slot.exchange(n, cpp::MemoryOrder::ACQ_REL);
    if (prev != nullptr)
      dom.retire(prev);
  }
  (void)skipped;
  return nullptr;
}

void run_scenario_7() {
  wstr("scenario 7: AdversarialReclaimRace\n");
  reset_pool_state<0>();
  g_s7_slot.store(nullptr, cpp::MemoryOrder::RELEASE);
  g_s7_publish_tag.store(1, cpp::MemoryOrder::RELEASE);

  pthread_t readers[kS7Readers];
  pthread_t retirers[kS7Retirers];
  for (unsigned i = 0; i < kS7Readers; ++i) {
    g_s7_args[i].idx = i;
    LIBC_NAMESPACE::pthread_create(&readers[i], nullptr, &s7_reader,
                                    &g_s7_args[i]);
  }
  for (unsigned i = 0; i < kS7Retirers; ++i) {
    g_s7_args[kS7Readers + i].idx = i;
    LIBC_NAMESPACE::pthread_create(&retirers[i], nullptr, &s7_retirer,
                                    &g_s7_args[kS7Readers + i]);
  }

  g_start.store(1, cpp::MemoryOrder::RELEASE);

  for (unsigned i = 0; i < kS7Readers; ++i)
    LIBC_NAMESPACE::pthread_join(readers[i], nullptr);
  for (unsigned i = 0; i < kS7Retirers; ++i)
    LIBC_NAMESPACE::pthread_join(retirers[i], nullptr);

  g_start.store(0, cpp::MemoryOrder::RELEASE);
  wstr("  pass\n");
}

} // namespace LIBC_NAMESPACE_DECL

// ====================================================================
// Entry point
// ====================================================================

extern "C" int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  using namespace LIBC_NAMESPACE;

  const char *sc_env = LIBC_NAMESPACE::getenv("CDS_SCENARIO");
  if (sc_env != nullptr) {
    unsigned v = 0;
    for (const char *p = sc_env; *p; ++p) {
      if (*p < '0' || *p > '9')
        break;
      v = v * 10 + static_cast<unsigned>(*p - '0');
    }
    if (v >= 1 && v <= 7)
      g_scenario = v;
  }

  const char *iters_env = LIBC_NAMESPACE::getenv("CDS_ITERS");
  if (iters_env != nullptr) {
    unsigned long v = 0;
    for (const char *p = iters_env; *p; ++p) {
      if (*p < '0' || *p > '9')
        break;
      v = v * 10 + static_cast<unsigned long>(*p - '0');
    }
    if (v > 0)
      g_iters_per_thread = v;
  }

  // Register the primary domain (used by scenarios 1-4 and 7).
  dom_of<0>().init_registration();

  switch (g_scenario) {
  case 1:
    run_scenario_1();
    break;
  case 2:
    run_scenario_2();
    break;
  case 3:
    run_scenario_3();
    break;
  case 4:
    run_scenario_4();
    break;
  case 5:
    run_scenario_5();
    break;
  case 6:
    run_scenario_6();
    break;
  case 7:
    run_scenario_7();
    break;
  default:
    wstr("unknown scenario\n");
    return 1;
  }

  wstr("OK\n");
  return 0;
}
