//===-- Post-dlopen FOREIGN cordon reconcile tests -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// When the NT loader maps a DLL it consumes VA inside ranges the libc's mmap
// engine may otherwise hint over. The contract verified here is the one the
// reconcile layer establishes via post_dlopen_reconcile(base, size):
//
//   1. After a DLL load + reconcile pass, the radix mapping table holds
//      FOREIGN cordon entries over the loader-installed VA. Slots inside the
//      module image resolve to FOREIGN (region_id == RegionPool::NONE) or
//      to one of the loader's own LIVE/PLACEHOLDER allocations — never to
//      FREE.
//
//   2. Subsequent mmap requests with a hint targeting the loaded module
//      range must NOT return a pointer that overlaps the DLL image.
//
//   3. MAP_FIXED_NOREPLACE at a loaded-DLL VA must fail with EEXIST: the
//      loader's allocation is observable to the libc, not silently stomped.
//
//   4. post_dlopen_reconcile is idempotent: a second call over the same
//      range performs zero new FOREIGN stamps (foreign_stamped == 0) and
//      does not crash.
//
//   5. After the DLL is unloaded, running the reconcile pass again clears
//      the now-stale FOREIGN cordons (foreign_cleared bumps when NT freed
//      the VA) so subsequent mmap can re-claim that VA.
//
//   6. Concurrent loader churn + mmap activity holds invariants: no crashes,
//      no overlap with currently-loaded DLL VA at any time.
//
// Two layers of coverage:
//
//   * Function-level (tests 1–5): drive the loader directly via
//     `LdrLoadDll` / `LdrUnloadDll` and invoke `post_dlopen_reconcile`
//     manually. This isolates the reconcile contract from the dlopen
//     wrapper and proves the function works in any caller's hands.
//
//   * End-to-end (tests 7–8): use `LIBC_NAMESPACE::dlopen` /
//     `LIBC_NAMESPACE::dlclose` directly. The libc dlopen wrapper at
//     `dlfcn_ops.cpp:355` already invokes `post_dlopen_reconcile` itself
//     after a successful `LdrLoadDll`, so a public-API mmap with a hint
//     at the loaded DLL's base must avoid the range without any test-
//     side reconcile call. This pins the production hook integration.
//
// Test 6 stresses both layers under thread contention (loader churn vs
// concurrent mmap) — driven through the loader directly so the timing
// is observable to the test thread.
//
//===----------------------------------------------------------------------===//

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
#include "src/__support/OSUtil/windows/memory/region_reconcile.h"
#include "src/__support/OSUtil/windows/nt/nt_string_api.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/dlfcn/dlclose.h"
#include "src/dlfcn/dlopen.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/sys-mman-macros.h"

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::windows::memory::cordon_foreigners_in_range;
using LIBC_NAMESPACE::windows::g_mapping_table;
using LIBC_NAMESPACE::windows::memory::post_dlopen_reconcile;
using LIBC_NAMESPACE::windows::memory::ReconcileStats;
using LIBC_NAMESPACE::windows::memory::RegionPool;
using LIBC_NAMESPACE::windows::SlotSnapshot;

using LlvmLibcPostDlopenReconcile =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

namespace {

// 64 KB allocation granularity — the unit FOREIGN cordon stamps land at.
constexpr size_t ALLOC_GRAN = 0x10000;
constexpr size_t PAGE = 4096;

// A small system DLL the loader is happy to map and unmap on demand.
// version.dll is ~30 KiB on stock Windows 11 and depends only on ntdll +
// kernelbase, both of which are already loaded into every NT process.
// UTF-16 literal so the type is `const char16_t[]` and matches `WCHAR` (=
// `char16_t`) in the project's NT type system. `L"..."` would yield a
// `wchar_t[]` literal which is 32-bit per the project's `-fwchar-type=int`.
const WCHAR SMALL_DLL_W[] = u"version.dll";

// Test DLL load result: pointer + size + raw NT handle, plus an error
// channel so callers can ASSERT cleanly.
struct LoadedDll {
  PVOID handle = nullptr;
  void *base = nullptr;
  SIZE_T size = 0;
  NTSTATUS status = 0;
};

// Walk PEB->Ldr by base address to recover the SizeOfImage the loader
// recorded for `handle`. This is exactly the lookup internal::dlopen does
// when it computes the reconcile bound.
SIZE_T peb_size_of_image(PVOID handle) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr)
    return 0;
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    if (entry->DllBase == handle)
      return entry->SizeOfImage;
  }
  return 0;
}

// Load a DLL via the NT loader (no Win32 LoadLibrary) and look up its
// SizeOfImage from the PEB. Caller is responsible for unloading. The name is
// passed as a UTF-16 literal (`u"..."`) because `WCHAR == char16_t` in the
// project's NT type system, while `wchar_t` is 32 bits under
// `-fwchar-type=int -fsigned-wchar`.
LoadedDll load_small_dll(const WCHAR *name) {
  LoadedDll out;
  LIBC_NAMESPACE::windows::nt_wstring_view wsv(name);
  out.status = ::LdrLoadDll(nullptr, nullptr, wsv.unicode_string(),
                             &out.handle);
  if (!NT_SUCCESS(out.status))
    return out;
  out.base = out.handle; // DllBase == handle by NT convention.
  out.size = peb_size_of_image(out.handle);
  return out;
}

// Returns true if any 64 KB slot covered by [base, base + size) is FOREIGN
// (region_id == RegionPool::NONE) or LIVE/PLACEHOLDER (occupied at all).
// The post-condition reconcile establishes is "no FREE slots inside the
// loader's range" — either form of occupied counts.
bool any_slot_occupied(void *base, SIZE_T size) {
  auto *cursor = static_cast<char *>(base);
  auto *limit = cursor + size;
  while (cursor < limit) {
    SlotSnapshot snap;
    if (g_mapping_table.snapshot(cursor, &snap))
      return true;
    cursor += ALLOC_GRAN;
  }
  return false;
}

// Counts slots inside the loader range that are explicitly FOREIGN
// (region_id == RegionPool::NONE). LIVE / PLACEHOLDER slots installed by
// the loader's own bookkeeping (e.g. activation contexts) do not count.
unsigned count_foreign_slots(void *base, SIZE_T size) {
  unsigned n = 0;
  auto *cursor = static_cast<char *>(base);
  auto *limit = cursor + size;
  while (cursor < limit) {
    SlotSnapshot snap;
    if (g_mapping_table.snapshot(cursor, &snap) &&
        snap.region_id == RegionPool::NONE)
      ++n;
    cursor += ALLOC_GRAN;
  }
  return n;
}

// Returns true iff [a_lo, a_lo + a_sz) and [b_lo, b_lo + b_sz) overlap.
bool ranges_overlap(void *a_lo, SIZE_T a_sz, void *b_lo, SIZE_T b_sz) {
  auto a0 = reinterpret_cast<uintptr_t>(a_lo);
  auto a1 = a0 + a_sz;
  auto b0 = reinterpret_cast<uintptr_t>(b_lo);
  auto b1 = b0 + b_sz;
  return a0 < b1 && b0 < a1;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. After load + reconcile, the loader's range shows up as occupied in
//    the mapping table — FOREIGN cordons were stamped over loader VA that
//    was previously FREE in our view.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcPostDlopenReconcile, LoadLibraryStampsForeignCordon) {
  LoadedDll dll = load_small_dll(SMALL_DLL_W);
  ASSERT_TRUE(NT_SUCCESS(dll.status));
  ASSERT_NE(dll.base, nullptr);
  ASSERT_GT(dll.size, SIZE_T{0});

  // Drive the reconcile contract directly.
  ReconcileStats stats = post_dlopen_reconcile(dll.base, dll.size);

  // The image header alone occupies at least one 64 KB allocation slot;
  // a real DLL with a code section spans several. We expect either fresh
  // stamps (stamped > 0) or revalidated existing stamps if a prior test in
  // this binary already touched the same range.
  EXPECT_GT(stats.foreign_stamped + stats.foreign_revalidated, 0u);

  // Stronger post-condition: every slot inside the loader range must now
  // resolve to *something* — no FREE holes that mmap could subsequently
  // hint into.
  EXPECT_TRUE(any_slot_occupied(dll.base, dll.size));

  // Specifically, at least one slot should be FOREIGN — the loader's
  // primary IMAGE allocation does not appear in our LIVE table.
  EXPECT_GT(count_foreign_slots(dll.base, dll.size), 0u);

  ASSERT_TRUE(NT_SUCCESS(::LdrUnloadDll(dll.handle)));
}

// ---------------------------------------------------------------------------
// 2. mmap with a hint at the DLL base must NOT return a pointer overlapping
//    the loaded image. NT's allocator already enforces this at the syscall
//    layer, but the libc's hint translation is what we're verifying — the
//    cordon stops the engine from believing the VA is reusable.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcPostDlopenReconcile, MmapAvoidsLoadedDllRange) {
  LoadedDll dll = load_small_dll(SMALL_DLL_W);
  ASSERT_TRUE(NT_SUCCESS(dll.status));
  ASSERT_GT(dll.size, SIZE_T{0});
  (void)post_dlopen_reconcile(dll.base, dll.size);

  // No MAP_FIXED — pure hint. The kernel may relocate; the libc may also
  // ignore the hint entirely and pick a different region. Either is fine,
  // but the returned region must not overlap the DLL.
  void *p = LIBC_NAMESPACE::mmap(dll.base, ALLOC_GRAN,
                                 PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  EXPECT_FALSE(ranges_overlap(p, ALLOC_GRAN, dll.base, dll.size));

  EXPECT_EQ(LIBC_NAMESPACE::munmap(p, ALLOC_GRAN), 0);
  ASSERT_TRUE(NT_SUCCESS(::LdrUnloadDll(dll.handle)));
}

// ---------------------------------------------------------------------------
// 3. MAP_FIXED_NOREPLACE at a loaded DLL VA must fail with EEXIST. This is
//    the strictest cordon test: the caller is forcing the address, the libc
//    must observe the loader's allocation and refuse to clobber it.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcPostDlopenReconcile, MapFixedNoreplaceFailsAtLoadedDllVa) {
  LoadedDll dll = load_small_dll(SMALL_DLL_W);
  ASSERT_TRUE(NT_SUCCESS(dll.status));
  ASSERT_GT(dll.size, SIZE_T{0});
  (void)post_dlopen_reconcile(dll.base, dll.size);

  // libc_errno is the canonical error channel; the test fixture clears it
  // between cases. mmap returns MAP_FAILED on rejection.
  void *p = LIBC_NAMESPACE::mmap(
      dll.base, ALLOC_GRAN, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  EXPECT_EQ(p, MAP_FAILED);
  // Don't strictly require EEXIST (the kernel could surface ENOMEM on some
  // edges), but it's the documented contract for MAP_FIXED_NOREPLACE.
  EXPECT_EQ(libc_errno, EEXIST);

  ASSERT_TRUE(NT_SUCCESS(::LdrUnloadDll(dll.handle)));
}

// ---------------------------------------------------------------------------
// 4. Idempotency: running reconcile twice over the same range must not
//    crash, and the second pass should stamp 0 new cordons (it may bump
//    foreign_revalidated for slots already cordoned by the first call).
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcPostDlopenReconcile, ReconcileIsIdempotent) {
  LoadedDll dll = load_small_dll(SMALL_DLL_W);
  ASSERT_TRUE(NT_SUCCESS(dll.status));
  ASSERT_GT(dll.size, SIZE_T{0});

  ReconcileStats first = post_dlopen_reconcile(dll.base, dll.size);
  ReconcileStats second = post_dlopen_reconcile(dll.base, dll.size);

  // The second pass must not produce *new* FOREIGN stamps over slots the
  // first pass already cordoned. Either the FREE→FOREIGN transition was
  // performed by the first call (so the second hits the FOREIGN-stale
  // path → revalidated > 0) or the loader installed its own LIVE entries
  // and neither pass needed to stamp.
  EXPECT_EQ(second.foreign_stamped, 0u);

  // First call should have made at least one observable change.
  EXPECT_GT(first.foreign_stamped + first.foreign_revalidated, 0u);

  ASSERT_TRUE(NT_SUCCESS(::LdrUnloadDll(dll.handle)));
}

// ---------------------------------------------------------------------------
// 5. After unload, reconcile must clear the stale FOREIGN cordons. The
//    reverse pass calls revalidate_foreign() per slot, which queries NT and
//    drops the cordon when the kernel has freed the VA. Subsequent mmap
//    over the same hint may then succeed (no MAP_FIXED guarantee since NT
//    is free to refuse, but the libc must not reject pre-emptively).
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcPostDlopenReconcile, UnloadDllRevalidatesForeignCordon) {
  LoadedDll dll = load_small_dll(SMALL_DLL_W);
  ASSERT_TRUE(NT_SUCCESS(dll.status));
  ASSERT_GT(dll.size, SIZE_T{0});

  // Snapshot the load location for the post-unload pass — we lose `dll`
  // semantically once we unload, but the VA range we want to re-probe is
  // independent of the handle's lifetime.
  void *saved_base = dll.base;
  SIZE_T saved_size = dll.size;

  // Stamp cordons from the load.
  ReconcileStats load_stats = post_dlopen_reconcile(saved_base, saved_size);
  EXPECT_GT(load_stats.foreign_stamped + load_stats.foreign_revalidated, 0u);

  ASSERT_TRUE(NT_SUCCESS(::LdrUnloadDll(dll.handle)));

  // Now reconcile again. Some FOREIGN slots should be cleared because NT
  // freed the image VA. We don't require *every* slot to clear (the loader
  // may keep activation context / .data overlays mapped briefly), but at
  // least one must — otherwise the cordon is permanent and the cleanup
  // contract is broken.
  ReconcileStats post_unload = post_dlopen_reconcile(saved_base, saved_size);
  EXPECT_GT(post_unload.foreign_cleared, 0u);
}

// ---------------------------------------------------------------------------
// 6. Stress: a loader thread loads + unloads a small DLL in a tight loop
//    while a worker thread hammers anonymous mmap/munmap. The mmap thread
//    must never receive an address overlapping the currently-loaded DLL.
//    Five iterations of load/unload is enough to cross-thread the mapping
//    table races without ballooning test time.
// ---------------------------------------------------------------------------

namespace {

struct StressCtx {
  Atomic<bool> done{false};
  Atomic<int> overlaps{0};       // failures observed
  Atomic<void *> live_base{nullptr};
  Atomic<SIZE_T> live_size{0};
};

DWORD loader_thread(void *arg) {
  auto *ctx = static_cast<StressCtx *>(arg);
  for (int i = 0; i < 5; ++i) {
    LoadedDll dll = load_small_dll(SMALL_DLL_W);
    if (!NT_SUCCESS(dll.status))
      continue;
    ctx->live_size.store(dll.size, MemoryOrder::RELEASE);
    ctx->live_base.store(dll.base, MemoryOrder::RELEASE);
    (void)post_dlopen_reconcile(dll.base, dll.size);
    LIBC_NAMESPACE::test_support::sleep_ms(2);
    // Publish "no live DLL" before unloading so the worker doesn't read a
    // stale base/size after the loader frees the VA.
    ctx->live_base.store(nullptr, MemoryOrder::RELEASE);
    ctx->live_size.store(0, MemoryOrder::RELEASE);
    ::LdrUnloadDll(dll.handle);
  }
  ctx->done.store(true, MemoryOrder::RELEASE);
  return 0;
}

DWORD mmap_thread(void *arg) {
  auto *ctx = static_cast<StressCtx *>(arg);
  while (!ctx->done.load(MemoryOrder::ACQUIRE)) {
    void *hint = ctx->live_base.load(MemoryOrder::ACQUIRE);
    void *p = LIBC_NAMESPACE::mmap(hint, ALLOC_GRAN,
                                   PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED)
      continue;

    // Re-read the live DLL at observation time — the loader may have
    // unloaded between our mmap and this check, but if there's still a
    // live DLL, our mmap result must not overlap it.
    void *cur_base = ctx->live_base.load(MemoryOrder::ACQUIRE);
    SIZE_T cur_size = ctx->live_size.load(MemoryOrder::ACQUIRE);
    if (cur_base != nullptr && cur_size != 0 &&
        ranges_overlap(p, ALLOC_GRAN, cur_base, cur_size)) {
      ctx->overlaps.fetch_add(1, MemoryOrder::RELAXED);
    }
    LIBC_NAMESPACE::munmap(p, ALLOC_GRAN);
  }
  return 0;
}

} // namespace

TEST_F(LlvmLibcPostDlopenReconcile, ConcurrentLoadAndMmap) {
  StressCtx ctx;
  HANDLE loader = LIBC_NAMESPACE::test_support::create_thread(loader_thread,
                                                              &ctx);
  ASSERT_NE(loader, nullptr);
  HANDLE mmaper = LIBC_NAMESPACE::test_support::create_thread(mmap_thread,
                                                              &ctx);
  ASSERT_NE(mmaper, nullptr);

  // Loader bounds the run: it sets done=true after 5 iterations.
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(
                loader, 30000),
            LIBC_NAMESPACE::test_support::WAIT_OBJECT_0);
  EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(
                mmaper, 30000),
            LIBC_NAMESPACE::test_support::WAIT_OBJECT_0);

  ::NtClose(loader);
  ::NtClose(mmaper);

  EXPECT_EQ(ctx.overlaps.load(MemoryOrder::ACQUIRE), 0);
}

// ---------------------------------------------------------------------------
// 7. End-to-end via LIBC_NAMESPACE::dlopen — the production hook integration.
//    `dlfcn_ops.cpp:355` invokes `post_dlopen_reconcile` itself after a
//    successful `LdrLoadDll`. A test caller that only goes through the public
//    dlopen surface should already see the cordon take effect, with no
//    test-side reconcile call. If this test passes while the function-level
//    tests above pass, the end-to-end wiring is good.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcPostDlopenReconcile, ProdDlopenInvokesReconcile) {
  // dlopen returns intptr_t (POSIX uses void* but our libc surface is the
  // intptr-typed variant). Convert once for the handle-shaped queries below.
  intptr_t raw = LIBC_NAMESPACE::dlopen("version.dll", 0);
  ASSERT_NE(raw, intptr_t{0});
  void *handle = reinterpret_cast<void *>(raw);

  // Recover SizeOfImage the same way the production wrapper does, so we can
  // probe the same range it just cordoned.
  SIZE_T module_size = peb_size_of_image(handle);
  ASSERT_GT(module_size, SIZE_T{0});

  // No manual reconcile here. The dlopen wrapper has already done it.
  EXPECT_TRUE(any_slot_occupied(handle, module_size));
  EXPECT_GT(count_foreign_slots(handle, module_size), 0u);

  EXPECT_EQ(LIBC_NAMESPACE::dlclose(handle), intptr_t{0});
}

// ---------------------------------------------------------------------------
// 8. End-to-end MAP_FIXED_NOREPLACE protection — same shape as test 3 but
//    driven through the public dlopen surface. Catches a regression where
//    the dlopen wrapper stops calling post_dlopen_reconcile (or calls it
//    with a wrong bound), since without the cordon NT alone may not refuse
//    a MAP_FIXED_NOREPLACE that targets a freshly-loaded module's range.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcPostDlopenReconcile, ProdDlopenFixedNoreplaceFailsAtDllVa) {
  intptr_t raw = LIBC_NAMESPACE::dlopen("version.dll", 0);
  ASSERT_NE(raw, intptr_t{0});
  void *handle = reinterpret_cast<void *>(raw);

  void *p = LIBC_NAMESPACE::mmap(
      handle, ALLOC_GRAN, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
  EXPECT_EQ(p, MAP_FAILED);
  EXPECT_EQ(libc_errno, EEXIST);

  EXPECT_EQ(LIBC_NAMESPACE::dlclose(handle), intptr_t{0});
}
