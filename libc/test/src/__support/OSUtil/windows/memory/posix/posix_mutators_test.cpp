//===-- POSIX-layer DescMutator-callback tests ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each test constructs a stack-resident RegionDesc, applies one mutator,
// and confirms (a) the target field changed, (b) unrelated `flags` bits
// stay intact, (c) the atomic store ordering produces values readable
// via ACQUIRE — the same fence the substrate's post-Swap reader uses.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/posix_mutators.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "test/UnitTest/Test.h"

#include <stdint.h>

namespace mp = LIBC_NAMESPACE::windows::memory_posix;
namespace rf = LIBC_NAMESPACE::windows::va_tracker::region_flag;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::windows::va_tracker::RegionDesc;

namespace {

// Seed every desc with a multi-bit flag pattern unrelated to whichever
// bit the mutator under test touches; assertions verify those bits
// survive intact.
constexpr uint16_t kSeedBits = rf::COMMITTED | rf::COW;

RegionDesc make_seeded_desc() {
  alignas(64) static thread_local unsigned char storage[sizeof(RegionDesc)];
  RegionDesc *d = new (storage) RegionDesc{};
  d->flags.store(kSeedBits, MemoryOrder::RELAXED);
  d->view_prot = PAGE_READONLY;
  d->numa_interleave_mask = 0;
  return *d;
}

} // namespace

TEST(LlvmLibcMemoryPosixMutatorsTest, ProtMutatorRewritesViewProt) {
  RegionDesc d = make_seeded_desc();
  DWORD new_prot = PAGE_READWRITE;
  mp::prot_mutator(&d, &new_prot);
  EXPECT_EQ(d.view_prot, static_cast<DWORD>(PAGE_READWRITE));
  EXPECT_EQ(d.flags.load(MemoryOrder::ACQUIRE), kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, LockMutatorIsNoopForFlags) {
  RegionDesc d = make_seeded_desc();
  bool locked = true;
  mp::lock_mutator(&d, &locked);
  EXPECT_EQ(d.flags.load(MemoryOrder::ACQUIRE), kSeedBits);
  EXPECT_EQ(d.view_prot, static_cast<DWORD>(PAGE_READONLY));
}

TEST(LlvmLibcMemoryPosixMutatorsTest, BrkExtendMutatorWritesCursor) {
  RegionDesc d = make_seeded_desc();
  void *cursor = reinterpret_cast<void *>(uintptr_t(0x1234'5000));
  mp::BrkExtendCtx ctx{cursor};
  mp::brk_extend_mutator(&d, &ctx);
  EXPECT_EQ(d.section_offset.QuadPart,
            static_cast<LONGLONG>(reinterpret_cast<uintptr_t>(cursor)));
  EXPECT_EQ(d.flags.load(MemoryOrder::ACQUIRE), kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, NumaRebindSetsFlagAndMask) {
  RegionDesc d = make_seeded_desc();
  mp::NumaRebindCtx ctx{true, 0xF};
  mp::numa_rebind_mutator(&d, &ctx);
  EXPECT_EQ(d.numa_interleave_mask, uint32_t(0xF));
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_TRUE((f & rf::NUMA_INTERLEAVE) != 0);
  EXPECT_TRUE((f & rf::COMMITTED) != 0);
  EXPECT_TRUE((f & rf::COW) != 0);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, NumaRebindClearsFlagOnDefault) {
  RegionDesc d = make_seeded_desc();
  d.flags.store(kSeedBits | rf::NUMA_INTERLEAVE, MemoryOrder::RELAXED);
  d.numa_interleave_mask = 0xFF;
  mp::NumaRebindCtx ctx{false, 0};
  mp::numa_rebind_mutator(&d, &ctx);
  EXPECT_EQ(d.numa_interleave_mask, uint32_t(0));
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_EQ(f & rf::NUMA_INTERLEAVE, 0);
  EXPECT_TRUE((f & rf::COMMITTED) != 0);
  EXPECT_TRUE((f & rf::COW) != 0);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, DumpSetSetsExcludeBit) {
  RegionDesc d = make_seeded_desc();
  mp::dump_set_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_TRUE((f & rf::DUMP_EXCLUDE) != 0);
  EXPECT_TRUE((f & rf::COMMITTED) != 0);
  EXPECT_TRUE((f & rf::COW) != 0);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, DumpClearClearsExcludeBit) {
  RegionDesc d = make_seeded_desc();
  d.flags.store(kSeedBits | rf::DUMP_EXCLUDE, MemoryOrder::RELAXED);
  mp::dump_clear_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_EQ(f & rf::DUMP_EXCLUDE, 0);
  EXPECT_EQ(f, kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, GuardSetSetsProtGuard) {
  RegionDesc d = make_seeded_desc();
  mp::guard_set_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_TRUE((f & rf::PROT_GUARD) != 0);
  EXPECT_EQ(f & kSeedBits, kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, GuardClearClearsProtGuard) {
  RegionDesc d = make_seeded_desc();
  d.flags.store(kSeedBits | rf::PROT_GUARD, MemoryOrder::RELAXED);
  mp::guard_clear_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_EQ(f & rf::PROT_GUARD, 0);
  EXPECT_EQ(f, kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, ForkSetDontforkSetsBit) {
  RegionDesc d = make_seeded_desc();
  mp::fork_set_dontfork_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_TRUE((f & rf::DONTFORK) != 0);
  EXPECT_EQ(f & kSeedBits, kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, ForkClearDontforkClearsBit) {
  RegionDesc d = make_seeded_desc();
  d.flags.store(kSeedBits | rf::DONTFORK, MemoryOrder::RELAXED);
  mp::fork_clear_dontfork_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_EQ(f & rf::DONTFORK, 0);
  EXPECT_EQ(f, kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, ForkSetWipeOnForkSetsBit) {
  RegionDesc d = make_seeded_desc();
  mp::fork_set_wipeonfork_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_TRUE((f & rf::WIPEONFORK) != 0);
  EXPECT_EQ(f & kSeedBits, kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, ForkClearWipeOnForkClearsBit) {
  RegionDesc d = make_seeded_desc();
  d.flags.store(kSeedBits | rf::WIPEONFORK, MemoryOrder::RELAXED);
  mp::fork_clear_wipeonfork_mutator(&d, nullptr);
  uint16_t f = d.flags.load(MemoryOrder::ACQUIRE);
  EXPECT_EQ(f & rf::WIPEONFORK, 0);
  EXPECT_EQ(f, kSeedBits);
}

TEST(LlvmLibcMemoryPosixMutatorsTest, MutatorsPreserveUnrelatedFlagBits) {
  // Cross-cutting check: every mutator that flips a single flag bit
  // leaves the other 15 bits intact. Walks all single-bit set/clear
  // mutators with a saturated seed.
  constexpr uint16_t kAllSeed =
      rf::COW | rf::SHARED | rf::HUGE_PAGES | rf::NORESERVE |
      rf::NUMA_INTERLEAVE | rf::COMMITTED | rf::LOW_32BIT;

  RegionDesc d = make_seeded_desc();
  d.flags.store(kAllSeed, MemoryOrder::RELAXED);
  mp::dump_set_mutator(&d, nullptr);
  EXPECT_EQ(d.flags.load(MemoryOrder::ACQUIRE) & kAllSeed, kAllSeed);
  mp::guard_set_mutator(&d, nullptr);
  EXPECT_EQ(d.flags.load(MemoryOrder::ACQUIRE) & kAllSeed, kAllSeed);
  mp::fork_set_dontfork_mutator(&d, nullptr);
  EXPECT_EQ(d.flags.load(MemoryOrder::ACQUIRE) & kAllSeed, kAllSeed);
  mp::fork_set_wipeonfork_mutator(&d, nullptr);
  EXPECT_EQ(d.flags.load(MemoryOrder::ACQUIRE) & kAllSeed, kAllSeed);
}
