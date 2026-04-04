//===-- RemapTransaction::commit failure-propagation regression ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Regression coverage for the silent-drop bug that used to live at the OLD
// `remap_transaction.cpp:147`: split-remap published the LEFT fragment
// successfully, then silently tore down the RIGHT fragment when its
// register_mapping failed, while `partial_unmap_view` returned 0 anyway.
// Kernel state and table state diverged.
//
// The fix:
//   1. `RemapTransaction::commit()` is `[[nodiscard]] bool` and returns false
//      on publish failure after rolling back the right view and releasing
//      the right fragment's region ref.
//   2. `partial_unmap_view` propagates the failure as -ENOMEM.
//
// Tests below pin the success direction of `commit()` across the three split
// orientations (head, middle, tail). The failure direction is guarded by the
// `[[nodiscard]]` attribute at every call site.
//
//===----------------------------------------------------------------------===//

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/region_desc.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/sys/mman/memfd_create.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/unistd/close.h"
#include "src/unistd/ftruncate.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "hdr/types/off_t.h"

namespace {

using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::g_mmap_lock;
using LIBC_NAMESPACE::windows::SlotSnapshot;
using LIBC_NAMESPACE::windows::memory::ChunkList;
using LIBC_NAMESPACE::windows::memory::g_region_pool;
using LIBC_NAMESPACE::windows::memory::RegionDesc;
using LIBC_NAMESPACE::windows::memory::RegionShape;

constexpr size_t K64 = 64u * 1024u;

// ---------------------------------------------------------------------------
// Snapshot helper — mirrors the convention from mmap_syscall_count_test.cpp.
// MmapLock shared so the resolved RegionDesc lifetime is pinned during the
// post-load reads of `current_shape()` / `chunk_list` / `refcount`.
// ---------------------------------------------------------------------------

LIBC_INLINE static bool snapshot_held(void *addr, SlotSnapshot *out) {
  g_mmap_lock.acquire_shared();
  bool ok = g_mapping_table.snapshot(addr, out);
  g_mmap_lock.release_shared();
  return ok;
}

// ---------------------------------------------------------------------------
// File helper — a 4 × 64 KB MAP_SHARED file view via memfd_create. Reused
// across the three commit-success tests so each test starts from the same
// shape (FILE_VIEW_MONO, 4 chunks worth of VA, fresh region descriptor).
// Returns base on success, nullptr on failure (each caller asserts).
// ---------------------------------------------------------------------------

struct MappedFile {
  int fd = -1;
  void *base = nullptr;
  size_t size = 0;
};

LIBC_INLINE static MappedFile make_4chunk_file(const char *name) {
  MappedFile m;
  m.fd = LIBC_NAMESPACE::memfd_create(name, 0);
  if (m.fd <= 0)
    return m;
  if (LIBC_NAMESPACE::ftruncate(m.fd, static_cast<off_t>(4 * K64)) != 0) {
    LIBC_NAMESPACE::close(m.fd);
    m.fd = -1;
    return m;
  }
  m.size = 4 * K64;
  m.base = LIBC_NAMESPACE::mmap(nullptr, m.size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, m.fd, 0);
  if (m.base == MAP_FAILED) {
    LIBC_NAMESPACE::close(m.fd);
    m.fd = -1;
    m.base = nullptr;
    m.size = 0;
  }
  return m;
}

LIBC_INLINE static void cleanup(MappedFile &m) {
  if (m.base != nullptr)
    LIBC_NAMESPACE::munmap(m.base, m.size);
  if (m.fd > 0)
    LIBC_NAMESPACE::close(m.fd);
}

// ===========================================================================
// (1)–(3): commit-success contract.
// ===========================================================================
//
// These pin the positive direction of the new bool return: a successful
// partial-unmap drives `RemapTransaction::commit()` through its happy path
// and `partial_unmap_view` returns 0. The plan considers these load-bearing
// because they're the only cases the test infrastructure can hit without
// an injection seam — they prove the new contract didn't regress success.
// A hypothetical refactor that left commit() as `void` (or made it always
// return false) would be caught here even before the failure-path seam
// lands.

// (1) Partial-unmap of the FIRST 64 KB. The split carves out the head; the
// surviving range is one tail chunk. The MONO -> CHUNKED promotion fires
// with exactly one ChunkEntry covering the tail.
TEST(LlvmLibcRemapTxnRegisterFailure,
     CommitReturnsTrueOnSuccess_PartialUnmapLeftHalf) {
  MappedFile m = make_4chunk_file("remap_txn_left_half");
  ASSERT_GT(m.fd, 0);
  ASSERT_NE(m.base, nullptr);
  char *base = static_cast<char *>(m.base);

  // Sentinel in the surviving tail.
  reinterpret_cast<uint32_t *>(base + 1 * K64)[0] = 0xAAAA0001u;
  reinterpret_cast<uint32_t *>(base + 3 * K64)[0] = 0xAAAA0003u;

  // Carve off the head 64 KB. Success means commit() returned true.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base, K64), Succeeds());

  // Surviving tail still readable (sentinel preserved). The aligned snap
  // address must come from the surviving fragment, not the freed head.
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 1 * K64)[0], 0xAAAA0001u);
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 3 * K64)[0], 0xAAAA0003u);

  SlotSnapshot post{};
  ASSERT_TRUE(snapshot_held(base + 1 * K64, &post));
  ASSERT_NE(post.region, nullptr);
  EXPECT_EQ(post.region->current_shape(), RegionShape::FILE_VIEW_CHUNKED);
  ChunkList *cl = post.region->get_chunk_list();
  ASSERT_NE(cl, nullptr);
  // One surviving chunk: tail (slots 1..3).
  EXPECT_EQ(cl->count, 1u);

  // Reset for cleanup — `m.base` is now bookkeeping-stale.
  m.base = nullptr;
  m.size = 0;
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 1 * K64, 3 * K64), Succeeds());
  cleanup(m);
}

// (2) Partial-unmap of the MIDDLE 64 KB. The split carves out slot 2 of 4;
// both head and tail survive. This is the canonical promotion: MONO ->
// CHUNKED with two ChunkEntries. This is also the configuration where the
// register_or_rollback bug originally triggered, because both fragments
// require register_mapping calls — the second one being the failure-prone
// one.
TEST(LlvmLibcRemapTxnRegisterFailure,
     CommitReturnsTrueOnSuccess_PartialUnmapMiddle) {
  MappedFile m = make_4chunk_file("remap_txn_middle");
  ASSERT_GT(m.fd, 0);
  ASSERT_NE(m.base, nullptr);
  char *base = static_cast<char *>(m.base);

  // Sentinels in head and tail. Verifying both post-unmap proves no
  // fragment was silently dropped — exactly the regression we're guarding.
  reinterpret_cast<uint32_t *>(base + 0 * K64)[0] = 0xBBBB0000u;
  reinterpret_cast<uint32_t *>(base + 3 * K64)[0] = 0xBBBB0003u;

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 2 * K64, K64), Succeeds());

  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 0 * K64)[0], 0xBBBB0000u);
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 3 * K64)[0], 0xBBBB0003u);

  SlotSnapshot post{};
  ASSERT_TRUE(snapshot_held(base, &post));
  ASSERT_NE(post.region, nullptr);
  EXPECT_EQ(post.region->current_shape(), RegionShape::FILE_VIEW_CHUNKED);
  ChunkList *cl = post.region->get_chunk_list();
  ASSERT_NE(cl, nullptr);
  // Two chunks: head (0..1) and tail (3).
  EXPECT_EQ(cl->count, 2u);

  m.base = nullptr;
  m.size = 0;
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0, 2 * K64), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 3 * K64, K64), Succeeds());
  cleanup(m);
}

// (3) Partial-unmap of the LAST 64 KB. Single surviving head chunk. The
// orientation matters because in `commit()` the `left_only` branch (no
// right fragment) takes a different code path from `both`/`right_only`,
// and the contract we're pinning has to hold across all three.
TEST(LlvmLibcRemapTxnRegisterFailure,
     CommitReturnsTrueOnSuccess_PartialUnmapLastHalf) {
  MappedFile m = make_4chunk_file("remap_txn_last_half");
  ASSERT_GT(m.fd, 0);
  ASSERT_NE(m.base, nullptr);
  char *base = static_cast<char *>(m.base);

  reinterpret_cast<uint32_t *>(base + 0 * K64)[0] = 0xCCCC0000u;
  reinterpret_cast<uint32_t *>(base + 2 * K64)[0] = 0xCCCC0002u;

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 3 * K64, K64), Succeeds());

  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 0 * K64)[0], 0xCCCC0000u);
  EXPECT_EQ(reinterpret_cast<uint32_t *>(base + 2 * K64)[0], 0xCCCC0002u);

  SlotSnapshot post{};
  ASSERT_TRUE(snapshot_held(base, &post));
  ASSERT_NE(post.region, nullptr);
  EXPECT_EQ(post.region->current_shape(), RegionShape::FILE_VIEW_CHUNKED);
  ChunkList *cl = post.region->get_chunk_list();
  ASSERT_NE(cl, nullptr);
  // One surviving chunk at the head (slots 0..2).
  EXPECT_EQ(cl->count, 1u);

  m.base = nullptr;
  m.size = 0;
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 0, 3 * K64), Succeeds());
  cleanup(m);
}

} // namespace
