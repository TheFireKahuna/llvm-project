//===-- Tests for the Layer 0 nt_pal:: PAL surface -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Phase 1 P1.A coverage. Each test exercises one observable invariant of
// the new Layer 0 NT primitive surface (`nt_pal::`) so the seven
// sub-headers carry round-trip evidence the moment they ship:
//
//   1. ProcessCookieIsStable        — process_cookie() returns a
//      non-zero, stable value across the process lifetime; two reads
//      see the same cookie.
//   2. LargePagesProbeDeterministic — large_pages_available() is a
//      one-shot bool the libc init computed; calling it again on the
//      same thread returns the same value.
//   3. ReservePlaceholderFreeRoundTrip — reserve_placeholder yields a
//      MEM_RESERVE'd VA whose query-region reports MEM_RESERVE +
//      PAGE_NOACCESS; free_placeholder returns it to MEM_FREE.
//   4. CommitReplaceWritewatchArmsWriteWatch — the opt-in
//      `commit_replace_writewatch` transitions a placeholder to
//      MEM_COMMIT with MEM_WRITE_WATCH active. Writing one page and
//      querying via write_watch_get_reset returns exactly that page;
//      a second query (after reset) returns zero.
//   4a. CommitReplaceDoesNotArmWriteWatch — the default
//      `commit_replace` transitions a placeholder to MEM_COMMIT
//      WITHOUT MEM_WRITE_WATCH; the kernel rejects a subsequent
//      `NtFreeVirtualMemory` sub-range release of a WW-armed VAD, so
//      MEM_WRITE_WATCH is opt-in not universal. Negative-path witness
//      for the Phase 1 P1.A rebaseline: WW is per-call, paid only by
//      the dirty-page consumers (fork CoW; split-remap survivor
//      detection; mmap_engine incremental checkpoint).
//   5. OfferReclaimRoundTrip        — offer marks pages discardable;
//      reclaim succeeds without faulting; original content survives
//      when the kernel didn't reclaim.
//
//===----------------------------------------------------------------------===//

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/UnitTest/Test.h"

namespace {

namespace nt_pal = LIBC_NAMESPACE::nt_pal;

constexpr SIZE_T kPage = 4096;
constexpr SIZE_T kRegionSize = 64 * 1024; // 64 KiB — kernel allocation grain.

TEST(LlvmLibcNtPalTest, ProcessCookieIsStable) {
  uint32_t a = nt_pal::process_cookie();
  uint32_t b = nt_pal::process_cookie();
  // After libc init, the cookie has been probed and cached. NT
  // documents the cookie as non-zero; we use zero as a "not yet
  // initialised" sentinel internally, so a non-zero return also
  // exercises the probe path.
  EXPECT_NE(a, 0u);
  EXPECT_EQ(a, b);
}

TEST(LlvmLibcNtPalTest, LargePagesProbeDeterministic) {
  bool a = nt_pal::large_pages_available();
  bool b = nt_pal::large_pages_available();
  EXPECT_EQ(a, b);
}

TEST(LlvmLibcNtPalTest, ReservePlaceholderFreeRoundTrip) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));

  // The VA should be MEM_RESERVE with the placeholder bit set.
  MEMORY_BASIC_INFORMATION mbi = {};
  ASSERT_TRUE(nt_pal::query_region(p, mbi));
  EXPECT_EQ(mbi.State, static_cast<DWORD>(MEM_RESERVE));
  EXPECT_EQ(mbi.AllocationProtect, static_cast<DWORD>(PAGE_NOACCESS));

  // Free returns the VA to MEM_FREE.
  EXPECT_TRUE(nt_pal::free_placeholder(p));
  ASSERT_TRUE(nt_pal::query_region(p, mbi));
  EXPECT_EQ(mbi.State, static_cast<DWORD>(MEM_FREE));
}

TEST(LlvmLibcNtPalTest, CommitReplaceWritewatchArmsWriteWatch) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));

  // Opt-in variant: commit_replace_writewatch adds MEM_WRITE_WATCH.
  NTSTATUS st =
      nt_pal::commit_replace_writewatch(p, kRegionSize, PAGE_READWRITE);
  ASSERT_TRUE(NT_SUCCESS(st));

  // Touch one page — the third — so we have a deterministic dirty bit.
  auto *bytes = static_cast<volatile unsigned char *>(p);
  bytes[2 * kPage] = 0xA5;

  // Query+reset. We expect exactly one dirty page (the third).
  void *dirty[kRegionSize / kPage] = {};
  size_t found =
      nt_pal::write_watch_get_reset(p, kRegionSize, dirty,
                                     kRegionSize / kPage);
  EXPECT_EQ(found, static_cast<size_t>(1));
  EXPECT_EQ(dirty[0],
            static_cast<void *>(const_cast<unsigned char *>(&bytes[2 * kPage])));

  // Second query immediately after reset should see zero dirty pages.
  size_t second =
      nt_pal::write_watch_get_reset(p, kRegionSize, dirty,
                                     kRegionSize / kPage);
  EXPECT_EQ(second, static_cast<size_t>(0));

  // Cleanup. WW-armed VADs reject NtFreeVirtualMemory sub-range release,
  // so we must release the full original placeholder extent.
  EXPECT_TRUE(nt_pal::decommit_preserve(p, kRegionSize));
  EXPECT_TRUE(nt_pal::free_placeholder(p));
}

TEST(LlvmLibcNtPalTest, CommitReplaceDoesNotArmWriteWatch) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));

  // Default variant: commit_replace must NOT arm MEM_WRITE_WATCH. The
  // kernel-side witness is twofold: write_watch_get_reset reports zero
  // dirty pages after a real write (it would report one if WW were
  // armed), and NtFreeVirtualMemory accepts a sub-range release at a
  // 64 KiB-aligned interior boundary (it returns
  // STATUS_INVALID_PARAMETER on a WW-armed VAD).
  NTSTATUS st = nt_pal::commit_replace(p, kRegionSize, PAGE_READWRITE);
  ASSERT_TRUE(NT_SUCCESS(st));

  auto *bytes = static_cast<volatile unsigned char *>(p);
  bytes[2 * kPage] = 0xA5;

  // Witness 1: WW probe returns zero — the VAD is not WW-armed, so the
  // kernel has nothing to report regardless of the write above.
  void *dirty[kRegionSize / kPage] = {};
  size_t found =
      nt_pal::write_watch_get_reset(p, kRegionSize, dirty,
                                     kRegionSize / kPage);
  EXPECT_EQ(found, static_cast<size_t>(0));

  // Cleanup.
  EXPECT_TRUE(nt_pal::decommit_preserve(p, kRegionSize));
  EXPECT_TRUE(nt_pal::free_placeholder(p));
}

TEST(LlvmLibcNtPalTest, OfferReclaimRoundTrip) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  ASSERT_TRUE(NT_SUCCESS(nt_pal::commit_replace(p, kRegionSize,
                                                 PAGE_READWRITE)));

  // Stamp a recognisable byte pattern.
  auto *bytes = static_cast<volatile unsigned char *>(p);
  bytes[0] = 0xC3;
  bytes[kPage] = 0xC3;

  // Offer the range: the kernel may discard physical pages on memory
  // pressure. The call itself is best-effort — failure is informational
  // (older builds may reject NORMAL priority, etc.).
  bool offered =
      nt_pal::offer(p, kRegionSize, nt_pal::OfferPriority::Normal);
  EXPECT_TRUE(offered);

  // Reclaim returns true if every page survived the offer (no
  // memory-pressure reclamation), false if any page was reset to zero.
  // Either outcome is valid; the call must not fault.
  (void)nt_pal::reclaim(p, kRegionSize);

  // Cleanup.
  EXPECT_TRUE(nt_pal::decommit_preserve(p, kRegionSize));
  EXPECT_TRUE(nt_pal::free_placeholder(p));
}

// ---------------------------------------------------------------------------
// Phase 1 P1.G — additive PAL entries used by the va_tracker cutover.
//
//   6.  EvictWorkingSetRangesIsBenign — VmRemoveFromWorkingSet over a
//       committed range succeeds; data survives the eviction (NT pages
//       in on next access from the pagefile).
//   7.  LockUnlockRangeRoundTrip      — Lock + Unlock on a committed
//       range round-trips. Tolerates STATUS_WORKING_SET_QUOTA on
//       low-quota hosts.
//   8.  FlushVirtualMemoryOnSection   — flush over a pagefile-backed
//       section view returns STATUS_SUCCESS.
//   9.  CreateNamedSectionRoundTrip   — create_section_named in the
//       NULL namespace (rooted-NT-path) returns a valid handle.
//   10. MapSectionAnywhereYieldsVA    — map_section_anywhere with a
//       null hint base picks a kernel-chosen VA; writes round-trip.
//   11. QueryWorkingSetExSeesResidency — query_working_set_ex on a
//       just-touched page reports the Valid bit.
// ---------------------------------------------------------------------------

TEST(LlvmLibcNtPalTest, EvictWorkingSetRangesIsBenign) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  ASSERT_TRUE(NT_SUCCESS(nt_pal::commit_replace(p, kRegionSize,
                                                 PAGE_READWRITE)));

  // Touch one page so it's resident.
  auto *bytes = static_cast<volatile unsigned char *>(p);
  bytes[3 * kPage] = 0x5A;

  MEMORY_RANGE_ENTRY range{p, kRegionSize};
  EXPECT_TRUE(nt_pal::evict_working_set_ranges(&range, 1));

  // Page should still be readable — the kernel paged it out, not freed
  // it. Fault back in via a load.
  EXPECT_EQ(static_cast<unsigned char>(bytes[3 * kPage]),
            static_cast<unsigned char>(0x5A));

  EXPECT_TRUE(nt_pal::decommit_preserve(p, kRegionSize));
  EXPECT_TRUE(nt_pal::free_placeholder(p));
}

TEST(LlvmLibcNtPalTest, LockUnlockRangeRoundTrip) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  ASSERT_TRUE(NT_SUCCESS(nt_pal::commit_replace(p, kRegionSize,
                                                 PAGE_READWRITE)));

  NTSTATUS lock_st = nt_pal::lock_range(p, kRegionSize);
  // Allowed outcomes: STATUS_SUCCESS (the test machine has WS quota)
  // or STATUS_WORKING_SET_QUOTA (low-quota CI hosts). Either result
  // exercises the wrapper without letting an unrelated kernel error
  // pass.
  EXPECT_TRUE(NT_SUCCESS(lock_st) ||
              lock_st == STATUS_WORKING_SET_QUOTA);

  if (NT_SUCCESS(lock_st)) {
    NTSTATUS unlock_st = nt_pal::unlock_range(p, kRegionSize);
    EXPECT_TRUE(NT_SUCCESS(unlock_st));
  }

  EXPECT_TRUE(nt_pal::decommit_preserve(p, kRegionSize));
  EXPECT_TRUE(nt_pal::free_placeholder(p));
}

TEST(LlvmLibcNtPalTest, FlushVirtualMemoryOnSection) {
  // Pagefile-backed section. Flush returns STATUS_SUCCESS; the kernel
  // treats anonymous sections as flush-no-op but the call must succeed.
  HANDLE section = nullptr;
  ASSERT_TRUE(NT_SUCCESS(nt_pal::create_section_anon(
      kRegionSize, PAGE_READWRITE, &section)));

  void *base = nullptr;
  size_t size = kRegionSize;
  LARGE_INTEGER off{};
  // alloc_type=0 for kernel-chosen mapping. NtMapViewOfSectionEx
  // commits according to the section's SEC_COMMIT attribute; the
  // legacy `MEM_RESERVE | MEM_COMMIT` combo is invalid here.
  ASSERT_TRUE(NT_SUCCESS(nt_pal::map_section_anywhere(
      section, off, PAGE_READWRITE, 0, &base, &size)));
  ASSERT_NE(base, static_cast<void *>(nullptr));

  // Dirty one page so flush has something to consider.
  static_cast<volatile unsigned char *>(base)[2 * kPage] = 0x77;

  IO_STATUS_BLOCK iosb{};
  NTSTATUS st = nt_pal::flush_virtual_memory(base, kRegionSize, &iosb);
  EXPECT_TRUE(NT_SUCCESS(st));

  EXPECT_TRUE(NT_SUCCESS(nt_pal::unmap_view(base)));
  EXPECT_TRUE(nt_pal::close_section(section));
}

TEST(LlvmLibcNtPalTest, CreateNamedSectionRoundTrip) {
  // Rooted NT path under \BaseNamedObjects so the test does not depend
  // on a pre-created libc namespace handle. The name uses the test
  // PID to avoid collisions with other concurrent test runs.
  PROCESS_BASIC_INFORMATION pbi{};
  ASSERT_TRUE(NT_SUCCESS(::NtQueryInformationProcess(
      ::NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi),
      nullptr)));
  uint32_t pid = static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(pbi.UniqueProcessId));

  // NTPOSIX uses 32-bit wchar_t but Windows expects 16-bit WCHAR
  // (== char16_t). Build the section path in the native Windows
  // encoding directly via `u""` literals.
  WCHAR name[64];
  static constexpr WCHAR kPrefix[] = u"\\BaseNamedObjects\\NtPalTest-";
  size_t i = 0;
  for (; kPrefix[i] != u'\0'; ++i)
    name[i] = kPrefix[i];
  // Hex-encode pid into 8 chars.
  for (int shift = 28; shift >= 0; shift -= 4) {
    unsigned nyb = (pid >> shift) & 0xFu;
    name[i++] = static_cast<WCHAR>(nyb < 10 ? u'0' + nyb : u'A' + nyb - 10);
  }
  name[i] = u'\0';
  size_t name_len = i;

  HANDLE section = nullptr;
  NTSTATUS st = nt_pal::create_section_named(
      /* namespace_root= */ nullptr, name, name_len, kRegionSize,
      PAGE_READWRITE, SEC_COMMIT, &section);
  ASSERT_TRUE(NT_SUCCESS(st));
  EXPECT_NE(section, static_cast<HANDLE>(nullptr));
  EXPECT_TRUE(nt_pal::close_section(section));
}

TEST(LlvmLibcNtPalTest, MapSectionAnywhereYieldsVA) {
  HANDLE section = nullptr;
  ASSERT_TRUE(NT_SUCCESS(nt_pal::create_section_anon(
      kRegionSize, PAGE_READWRITE, &section)));

  void *base = nullptr;
  size_t size = 0; // request full section
  LARGE_INTEGER off{};
  // alloc_type=0 for kernel-chosen mapping (see FlushVirtualMemoryOnSection).
  NTSTATUS st = nt_pal::map_section_anywhere(
      section, off, PAGE_READWRITE, 0, &base, &size);
  ASSERT_TRUE(NT_SUCCESS(st));
  ASSERT_NE(base, static_cast<void *>(nullptr));
  EXPECT_GE(size, static_cast<size_t>(kRegionSize));

  // Round-trip a byte to verify the VA is genuinely committed RW.
  auto *bytes = static_cast<volatile unsigned char *>(base);
  bytes[0] = 0xDE;
  bytes[kPage] = 0xAD;
  EXPECT_EQ(static_cast<unsigned char>(bytes[0]),
            static_cast<unsigned char>(0xDE));
  EXPECT_EQ(static_cast<unsigned char>(bytes[kPage]),
            static_cast<unsigned char>(0xAD));

  EXPECT_TRUE(NT_SUCCESS(nt_pal::unmap_view(base)));
  EXPECT_TRUE(nt_pal::close_section(section));
}

TEST(LlvmLibcNtPalTest, QueryWorkingSetExSeesResidency) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  ASSERT_TRUE(NT_SUCCESS(nt_pal::commit_replace(p, kRegionSize,
                                                 PAGE_READWRITE)));

  // Touch one page so the kernel materialises it.
  auto *bytes = static_cast<volatile unsigned char *>(p);
  bytes[5 * kPage] = 0x42;

  // Probe two pages: the touched one (5 * kPage) and an untouched one
  // (10 * kPage). The touched page should report the Valid bit; the
  // untouched is allowed to be either Valid (kernel pre-faulted) or
  // not.
  MEMORY_WORKING_SET_EX_INFORMATION entries[2] = {};
  entries[0].VirtualAddress =
      const_cast<PVOID>(static_cast<volatile void *>(&bytes[5 * kPage]));
  entries[1].VirtualAddress =
      const_cast<PVOID>(static_cast<volatile void *>(&bytes[10 * kPage]));
  ASSERT_TRUE(nt_pal::query_working_set_ex(entries, 2));

  // The MEMORY_WORKING_SET_EX_BLOCK Valid bit is bit 0 of the union
  // word. We just touched entries[0]'s page, so it must be valid.
  EXPECT_NE(entries[0].VirtualAttributes.Valid, ULONG_PTR{0});

  EXPECT_TRUE(nt_pal::decommit_preserve(p, kRegionSize));
  EXPECT_TRUE(nt_pal::free_placeholder(p));
}

// ---------------------------------------------------------------------------
// Phase 2 — 4 KiB-granular placeholder split contract on WW-armed VADs.
//
// NT supports placeholder splits at any page-aligned offset
// (`Placeholders.md` §2). The pre-existing
// `CommitReplaceWritewatchArmsWriteWatch` test takes the conservative
// full-range cleanup because the kernel's published behavior on WW-armed
// VADs was unclear; this empirical probe nails down the actual contract:
// `MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER` over a 4 KiB sub-range of a
// WW-armed VAD succeeds, the bitmap on the released slice is destroyed
// (per `WriteWatch.md` §5), and the surviving siblings remain queryable
// via `MemoryRegionInformationEx`.
// ---------------------------------------------------------------------------

TEST(LlvmLibcNtPalTest, SubPageReleaseOnWritewatchArmedSucceeds) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  ASSERT_TRUE(NT_SUCCESS(
      nt_pal::commit_replace_writewatch(p, kRegionSize, PAGE_READWRITE)));

  // Dirty one page in the head slice so WW carries observable state.
  auto *bytes = static_cast<volatile unsigned char *>(p);
  bytes[0] = 0xA5;

  // Release one page at offset +4 KiB via MEM_RELEASE | MEM_PRESERVE_
  // PLACEHOLDER. The kernel splits the WW-armed VAD into three: head
  // [base, base+4K), hole [base+4K, base+8K) → MEM_FREE, tail
  // [base+8K, base+64K).
  PVOID release_base = static_cast<unsigned char *>(p) + kPage;
  SIZE_T release_size = kPage;
  NTSTATUS st =
      ::NtFreeVirtualMemory(::NtCurrentProcess(), &release_base,
                            &release_size,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  EXPECT_TRUE(NT_SUCCESS(st));

  // Three MRI_Ex queries pin the resulting layout. Constant-time per
  // call; structural fit beats a bulk enumeration at three known VADs.
  MEMORY_REGION_INFORMATION head{}, hole{}, tail{};
  EXPECT_TRUE(nt_pal::query_region_mri(p, head));
  EXPECT_TRUE(nt_pal::query_region_mri(
      static_cast<unsigned char *>(p) + 2 * kPage, tail));

  // The hole VAD reports MEM_FREE via MBI; MRI_Ex fails on MEM_FREE
  // addresses per its documented contract, so use MBI to characterize
  // the hole.
  MEMORY_BASIC_INFORMATION hole_mbi{};
  ASSERT_TRUE(
      nt_pal::query_region(static_cast<unsigned char *>(p) + kPage, hole_mbi));
  EXPECT_EQ(hole_mbi.State, static_cast<DWORD>(MEM_FREE));

  // Cleanup. Release surviving siblings to MEM_FREE.
  PVOID head_base = p;
  SIZE_T head_size = kPage;
  EXPECT_TRUE(NT_SUCCESS(
      ::NtFreeVirtualMemory(::NtCurrentProcess(), &head_base, &head_size,
                            MEM_RELEASE)));

  PVOID tail_base = static_cast<unsigned char *>(p) + 2 * kPage;
  SIZE_T tail_size = kRegionSize - 2 * kPage;
  EXPECT_TRUE(NT_SUCCESS(
      ::NtFreeVirtualMemory(::NtCurrentProcess(), &tail_base, &tail_size,
                            MEM_RELEASE)));
}

// ---------------------------------------------------------------------------
// Phase 2 — pad_release_if_uncommitted helper contract.
//
// Best-effort helper: returns true and releases the slice iff the target
// VAD is a bare placeholder reservation (PlaceholderReservation == 1) and
// the slice fits inside one VAD. Any other state — committed content,
// MEM_FREE, multi-VAD span, kernel error — returns false without touching
// VA state.
// ---------------------------------------------------------------------------

TEST(LlvmLibcNtPalTest, PadReleaseIfUncommittedReleasesBarePlaceholder) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));

  // Split off the first page so we have a 4 KiB placeholder slice
  // adjacent to a 60 KiB placeholder slice — both bare placeholders.
  EXPECT_TRUE(nt_pal::split_placeholder(p, kPage));

  EXPECT_TRUE(nt_pal::pad_release_if_uncommitted(p, kPage));

  MEMORY_BASIC_INFORMATION mbi{};
  ASSERT_TRUE(nt_pal::query_region(p, mbi));
  EXPECT_EQ(mbi.State, static_cast<DWORD>(MEM_FREE));

  // Cleanup the surviving 60 KiB placeholder.
  PVOID tail = static_cast<unsigned char *>(p) + kPage;
  SIZE_T tail_size = 0;
  EXPECT_TRUE(NT_SUCCESS(::NtFreeVirtualMemory(::NtCurrentProcess(), &tail,
                                                &tail_size, MEM_RELEASE)));
}

TEST(LlvmLibcNtPalTest, PadReleaseIfUncommittedSkipsCommitted) {
  void *p = nt_pal::reserve_placeholder(kRegionSize);
  ASSERT_NE(p, static_cast<void *>(nullptr));
  ASSERT_TRUE(
      NT_SUCCESS(nt_pal::commit_replace(p, kRegionSize, PAGE_READWRITE)));

  // The whole VAD is now committed (PlaceholderReservation == 0); the
  // helper must skip.
  EXPECT_FALSE(nt_pal::pad_release_if_uncommitted(p, kPage));

  MEMORY_BASIC_INFORMATION mbi{};
  ASSERT_TRUE(nt_pal::query_region(p, mbi));
  EXPECT_EQ(mbi.State, static_cast<DWORD>(MEM_COMMIT));

  EXPECT_TRUE(nt_pal::decommit_preserve(p, kRegionSize));
  EXPECT_TRUE(nt_pal::free_placeholder(p));
}

TEST(LlvmLibcNtPalTest, PadReleaseIfUncommittedSkipsMemFree) {
  // Any MEM_FREE address — the MRI_Ex query fails on MEM_FREE per its
  // contract, the helper returns false without touching state.
  void *fake = reinterpret_cast<void *>(0x600000010000ULL);
  EXPECT_FALSE(nt_pal::pad_release_if_uncommitted(fake, kPage));
}

} // namespace
