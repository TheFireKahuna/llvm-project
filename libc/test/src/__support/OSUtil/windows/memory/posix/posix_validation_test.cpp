//===-- POSIX-layer validation-helper tests -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/windows/sys-mman-macros.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

namespace mp = LIBC_NAMESPACE::windows::memory_posix;

//===----------------------------------------------------------------------===//
// Page / alignment helpers.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, RoundUpToPageAlignsPositive) {
  // 1 byte rounds up to one page; an exact page stays unchanged.
  EXPECT_TRUE(mp::round_up_to_page(1) >= 4096u);
  EXPECT_EQ(mp::round_up_to_page(mp::round_up_to_page(1)),
            mp::round_up_to_page(1));
}

TEST(LlvmLibcMemoryPosixValidationTest, RoundUpToPageOverflowReturnsZero) {
  EXPECT_EQ(mp::round_up_to_page(UINTPTR_MAX), uintptr_t(0));
}

TEST(LlvmLibcMemoryPosixValidationTest, IsPageAlignedRecognisesNull) {
  EXPECT_TRUE(mp::is_page_aligned(nullptr));
}

TEST(LlvmLibcMemoryPosixValidationTest, IsPageAlignedRejectsOffByOne) {
  void *p = reinterpret_cast<void *>(uintptr_t(1));
  EXPECT_FALSE(mp::is_page_aligned(p));
}

//===----------------------------------------------------------------------===//
// Overflow checks.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, RoundedLenZeroOnZeroInput) {
  EXPECT_EQ(mp::rounded_len_or_zero(0), size_t(0));
}

TEST(LlvmLibcMemoryPosixValidationTest, RoundedLenZeroOnOverflow) {
  EXPECT_EQ(mp::rounded_len_or_zero(SIZE_MAX), size_t(0));
}

TEST(LlvmLibcMemoryPosixValidationTest, RoundedLenRoundsUp) {
  EXPECT_TRUE(mp::rounded_len_or_zero(1) >= 4096u);
}

TEST(LlvmLibcMemoryPosixValidationTest, AddrPlusLenOverflowDetected) {
  EXPECT_TRUE(mp::addr_plus_len_overflows(UINTPTR_MAX, 1));
  EXPECT_TRUE(mp::addr_plus_len_overflows(UINTPTR_MAX - 4, 8));
}

TEST(LlvmLibcMemoryPosixValidationTest, AddrPlusLenInBoundsAccepted) {
  EXPECT_FALSE(mp::addr_plus_len_overflows(0, 4096));
  EXPECT_FALSE(mp::addr_plus_len_overflows(1, 0));
}

//===----------------------------------------------------------------------===//
// Protection validation.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, ProtRejectsUnknownBits) {
  EXPECT_EQ(mp::validate_posix_prot(PROT_READ | 0x80, MAP_PRIVATE), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, ProtRejectsWPlusXWithoutMapWx) {
  EXPECT_EQ(mp::validate_posix_prot(PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE),
            EACCES);
}

TEST(LlvmLibcMemoryPosixValidationTest, ProtAcceptsWPlusXWithMapWx) {
  EXPECT_EQ(mp::validate_posix_prot(PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_WX),
            0);
}

TEST(LlvmLibcMemoryPosixValidationTest, ProtRejectsExecOnlyAsNotsup) {
  EXPECT_EQ(mp::validate_posix_prot(PROT_EXEC, MAP_PRIVATE), ENOTSUP);
}

TEST(LlvmLibcMemoryPosixValidationTest, ProtAcceptsValidCombinations) {
  EXPECT_EQ(mp::validate_posix_prot(PROT_NONE, MAP_PRIVATE), 0);
  EXPECT_EQ(mp::validate_posix_prot(PROT_READ, MAP_PRIVATE), 0);
  EXPECT_EQ(mp::validate_posix_prot(PROT_READ | PROT_WRITE, MAP_PRIVATE), 0);
  EXPECT_EQ(mp::validate_posix_prot(PROT_READ | PROT_EXEC, MAP_PRIVATE), 0);
}

TEST(LlvmLibcMemoryPosixValidationTest, MprotectRejectsWPlusX) {
  // mprotect never permits W+X — no MAP_WX opt-in path.
  EXPECT_EQ(mp::validate_mprotect_prot(PROT_READ | PROT_WRITE | PROT_EXEC),
            EACCES);
}

TEST(LlvmLibcMemoryPosixValidationTest, MprotectRejectsExecOnly) {
  EXPECT_EQ(mp::validate_mprotect_prot(PROT_EXEC), ENOTSUP);
}

//===----------------------------------------------------------------------===//
// mmap flag validation.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, MmapFlagsRejectBothSharedPrivate) {
  EXPECT_EQ(mp::validate_mmap_flags(MAP_SHARED | MAP_PRIVATE), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MmapFlagsRejectNeitherSharedPrivate) {
  EXPECT_EQ(mp::validate_mmap_flags(0), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MmapFlagsRejectGrowsdownNumeric) {
  EXPECT_EQ(mp::validate_mmap_flags(MAP_PRIVATE | mp::kMapGrowsdownNumericValue),
            EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MmapFlagsRejectSyncNumeric) {
  EXPECT_EQ(mp::validate_mmap_flags(MAP_PRIVATE | mp::kMapSyncNumericValue),
            EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MmapFlagsAcceptValidCombo) {
  EXPECT_EQ(mp::validate_mmap_flags(MAP_PRIVATE | MAP_ANONYMOUS), 0);
  EXPECT_EQ(mp::validate_mmap_flags(MAP_SHARED), 0);
}

TEST(LlvmLibcMemoryPosixValidationTest, FixedNoreplaceRejectsNullAddr) {
  EXPECT_EQ(mp::validate_fixed_noreplace_addr(MAP_FIXED_NOREPLACE, nullptr),
            EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, FixedNoreplaceRejectsUnalignedAddr) {
  void *p = reinterpret_cast<void *>(uintptr_t(0x1001));
  EXPECT_EQ(mp::validate_fixed_noreplace_addr(MAP_FIXED_NOREPLACE, p), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, FixedNoreplaceIgnoredIfFlagAbsent) {
  EXPECT_EQ(mp::validate_fixed_noreplace_addr(0, nullptr), 0);
}

//===----------------------------------------------------------------------===//
// mremap flag-matrix.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, MremapRejectsUnknownBits) {
  EXPECT_EQ(mp::validate_mremap_flags(0x10000, 4096, 4096), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MremapRejectsDontunmapWithoutMaymove) {
  EXPECT_EQ(mp::validate_mremap_flags(MREMAP_DONTUNMAP, 4096, 4096), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MremapRejectsDontunmapSizeChange) {
  EXPECT_EQ(mp::validate_mremap_flags(MREMAP_MAYMOVE | MREMAP_DONTUNMAP, 4096,
                                      8192),
            EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MremapRejectsFixedWithoutMaymove) {
  EXPECT_EQ(mp::validate_mremap_flags(MREMAP_FIXED, 4096, 4096), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MremapAcceptsValidCombinations) {
  EXPECT_EQ(mp::validate_mremap_flags(0, 4096, 4096), 0);
  EXPECT_EQ(mp::validate_mremap_flags(MREMAP_MAYMOVE, 4096, 8192), 0);
  EXPECT_EQ(mp::validate_mremap_flags(MREMAP_MAYMOVE | MREMAP_FIXED, 4096,
                                      8192),
            0);
  EXPECT_EQ(mp::validate_mremap_flags(MREMAP_MAYMOVE | MREMAP_DONTUNMAP, 4096,
                                      4096),
            0);
}

//===----------------------------------------------------------------------===//
// msync flags.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, MsyncRejectsBothAsyncSync) {
  EXPECT_EQ(mp::validate_msync_flags(MS_ASYNC | MS_SYNC), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MsyncRejectsNeitherAsyncSync) {
  EXPECT_EQ(mp::validate_msync_flags(0), EINVAL);
  EXPECT_EQ(mp::validate_msync_flags(MS_INVALIDATE), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MsyncRejectsUnknownBits) {
  EXPECT_EQ(mp::validate_msync_flags(MS_ASYNC | 0x80), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MsyncAcceptsValidCombinations) {
  EXPECT_EQ(mp::validate_msync_flags(MS_ASYNC), 0);
  EXPECT_EQ(mp::validate_msync_flags(MS_SYNC), 0);
  EXPECT_EQ(mp::validate_msync_flags(MS_ASYNC | MS_INVALIDATE), 0);
  EXPECT_EQ(mp::validate_msync_flags(MS_SYNC | MS_INVALIDATE), 0);
}

//===----------------------------------------------------------------------===//
// madvise advice recognition + entry validation.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, MadviseRecognisesKnownCodes) {
  EXPECT_TRUE(mp::madvise_advice_recognised(MADV_NORMAL));
  EXPECT_TRUE(mp::madvise_advice_recognised(MADV_DONTNEED));
  EXPECT_TRUE(mp::madvise_advice_recognised(MADV_FREE));
  EXPECT_TRUE(mp::madvise_advice_recognised(MADV_DONTDUMP));
  EXPECT_TRUE(mp::madvise_advice_recognised(MADV_DODUMP));
  EXPECT_TRUE(mp::madvise_advice_recognised(MADV_POPULATE_WRITE));
  EXPECT_TRUE(mp::madvise_advice_recognised(mp::kMadviseRemove));
  EXPECT_TRUE(mp::madvise_advice_recognised(mp::kMadviseDontfork));
  EXPECT_TRUE(mp::madvise_advice_recognised(mp::kMadviseWipeOnFork));
  EXPECT_TRUE(mp::madvise_advice_recognised(mp::kMadviseHwpoison));
  EXPECT_TRUE(mp::madvise_advice_recognised(mp::kMadviseGuardInstall));
}

TEST(LlvmLibcMemoryPosixValidationTest, MadviseRejectsUnknownCodes) {
  EXPECT_FALSE(mp::madvise_advice_recognised(999));
  EXPECT_FALSE(mp::madvise_advice_recognised(-1));
}

TEST(LlvmLibcMemoryPosixValidationTest, MadviseEntryRejectsNullAsEnomem) {
  size_t rounded = 0;
  EXPECT_EQ(mp::validate_madvise_entry(nullptr, 4096, MADV_DONTNEED, &rounded),
            ENOMEM);
}

TEST(LlvmLibcMemoryPosixValidationTest, MadviseEntryRejectsUnalignedAddr) {
  void *p = reinterpret_cast<void *>(uintptr_t(0x1001));
  size_t rounded = 0;
  EXPECT_EQ(mp::validate_madvise_entry(p, 4096, MADV_DONTNEED, &rounded),
            EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MadviseEntryZeroSizeSucceeds) {
  void *p = reinterpret_cast<void *>(uintptr_t(0x1000));
  size_t rounded = 12345;
  EXPECT_EQ(mp::validate_madvise_entry(p, 0, MADV_DONTNEED, &rounded), 0);
  EXPECT_EQ(rounded, size_t(0));
}

TEST(LlvmLibcMemoryPosixValidationTest, MadviseEntryUnknownAdviceEinval) {
  void *p = reinterpret_cast<void *>(uintptr_t(0x1000));
  size_t rounded = 0;
  EXPECT_EQ(mp::validate_madvise_entry(p, 4096, 9999, &rounded), EINVAL);
}

TEST(LlvmLibcMemoryPosixValidationTest, MadviseEntrySuccessRoundsLen) {
  void *p = reinterpret_cast<void *>(uintptr_t(0x1000));
  size_t rounded = 0;
  EXPECT_EQ(mp::validate_madvise_entry(p, 1, MADV_DONTNEED, &rounded), 0);
  EXPECT_TRUE(rounded >= 4096u);
}

//===----------------------------------------------------------------------===//
// prot → PAGE_* lookup.
//===----------------------------------------------------------------------===//

TEST(LlvmLibcMemoryPosixValidationTest, ProtToPageNoneIsNoaccess) {
  EXPECT_EQ(mp::posix_prot_to_page(PROT_NONE),
            static_cast<DWORD>(PAGE_NOACCESS));
}

TEST(LlvmLibcMemoryPosixValidationTest, ProtToPageReadWriteIsReadwrite) {
  EXPECT_EQ(mp::posix_prot_to_page(PROT_READ | PROT_WRITE),
            static_cast<DWORD>(PAGE_READWRITE));
}

TEST(LlvmLibcMemoryPosixValidationTest, ProtToPageCowMakesWriteIntoWritecopy) {
  EXPECT_EQ(mp::posix_prot_to_page_cow(PROT_READ | PROT_WRITE),
            static_cast<DWORD>(PAGE_WRITECOPY));
}
