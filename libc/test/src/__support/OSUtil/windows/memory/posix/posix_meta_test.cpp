//===-- POSIX-layer AcquireMeta-builder tests -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"

#include "include/llvm-libc-macros/windows/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>

namespace mp = LIBC_NAMESPACE::windows::memory_posix;
namespace rf = LIBC_NAMESPACE::windows::va_tracker::region_flag;

TEST(LlvmLibcMemoryPosixMetaTest, AnonPrivateMetaSetsCommittedNoShared) {
  auto m = mp::anon_private_meta(PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS);
  EXPECT_EQ(m.view_prot, static_cast<DWORD>(PAGE_READWRITE));
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.flags & rf::SHARED, 0);
  EXPECT_EQ(m.flags & rf::NORESERVE, 0);
  EXPECT_EQ(m.flags & rf::COW, 0);
  EXPECT_EQ(m.section_handle, nullptr);
  EXPECT_EQ(m.file_handle, nullptr);
}

TEST(LlvmLibcMemoryPosixMetaTest, AnonPrivateNoreserveDropsCommitted) {
  auto m = mp::anon_private_meta(PROT_READ,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE);
  EXPECT_TRUE((m.flags & rf::NORESERVE) != 0);
  EXPECT_EQ(m.flags & rf::COMMITTED, 0);
}

TEST(LlvmLibcMemoryPosixMetaTest, AnonPrivateMap32BitSetsLow32Bit) {
  auto m = mp::anon_private_meta(PROT_READ,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT);
  EXPECT_TRUE((m.flags & rf::LOW_32BIT) != 0);
}

TEST(LlvmLibcMemoryPosixMetaTest, AnonSharedMetaSetsSharedAndSection) {
  HANDLE fake_section = reinterpret_cast<HANDLE>(uintptr_t(0xDEAD));
  auto m = mp::anon_shared_meta(PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, fake_section);
  EXPECT_TRUE((m.flags & rf::SHARED) != 0);
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.section_handle, fake_section);
  EXPECT_EQ(m.flags & rf::COW, 0);
}

TEST(LlvmLibcMemoryPosixMetaTest, FilePrivateMetaUsesWritecopy) {
  HANDLE sec = reinterpret_cast<HANDLE>(uintptr_t(0xBEEF));
  HANDLE file = reinterpret_cast<HANDLE>(uintptr_t(0xCAFE));
  auto m = mp::file_private_meta(PROT_READ | PROT_WRITE, MAP_PRIVATE, sec,
                                  file, 4096);
  EXPECT_EQ(m.view_prot, static_cast<DWORD>(PAGE_WRITECOPY));
  EXPECT_TRUE((m.flags & rf::COW) != 0);
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.flags & rf::SHARED, 0);
  EXPECT_EQ(m.section_handle, sec);
  EXPECT_EQ(m.file_handle, file);
  EXPECT_EQ(m.section_offset, uint64_t(4096));
}

TEST(LlvmLibcMemoryPosixMetaTest, FileSharedMetaSkipsCow) {
  HANDLE sec = reinterpret_cast<HANDLE>(uintptr_t(0xBEEF));
  HANDLE file = reinterpret_cast<HANDLE>(uintptr_t(0xCAFE));
  auto m = mp::file_shared_meta(PROT_READ | PROT_WRITE, MAP_SHARED, sec, file,
                                 0);
  EXPECT_EQ(m.view_prot, static_cast<DWORD>(PAGE_READWRITE));
  EXPECT_TRUE((m.flags & rf::SHARED) != 0);
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.flags & rf::COW, 0);
}

TEST(LlvmLibcMemoryPosixMetaTest, HugetlbMetaSetsHugePages) {
  HANDLE sec = reinterpret_cast<HANDLE>(uintptr_t(0xF00D));
  auto m = mp::hugetlb_meta(PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                             LIBC_NAMESPACE::nt_pal::LargePageKind::Large, sec);
  EXPECT_TRUE((m.flags & rf::HUGE_PAGES) != 0);
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.section_handle, sec);
}

TEST(LlvmLibcMemoryPosixMetaTest, BrkMetaCarriesPlaceholderIdentity) {
  void *base = reinterpret_cast<void *>(uintptr_t(0x10000));
  auto m = mp::brk_meta(PROT_READ | PROT_WRITE, base, size_t(256u << 20));
  EXPECT_EQ(m.placeholder_base, base);
  EXPECT_EQ(m.placeholder_size, size_t(256u << 20));
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.view_prot, static_cast<DWORD>(PAGE_READWRITE));
}

TEST(LlvmLibcMemoryPosixMetaTest, ShmPosixMetaShared) {
  HANDLE sec = reinterpret_cast<HANDLE>(uintptr_t(0xABCD));
  auto m = mp::shm_posix_meta(PROT_READ | PROT_WRITE, MAP_SHARED, sec);
  EXPECT_TRUE((m.flags & rf::SHARED) != 0);
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.section_handle, sec);
}

TEST(LlvmLibcMemoryPosixMetaTest, ShmSysVMetaShared) {
  HANDLE sec = reinterpret_cast<HANDLE>(uintptr_t(0xABCD));
  auto m = mp::shm_sysv_meta(PROT_READ, MAP_SHARED, sec);
  EXPECT_TRUE((m.flags & rf::SHARED) != 0);
  EXPECT_TRUE((m.flags & rf::COMMITTED) != 0);
  EXPECT_EQ(m.section_handle, sec);
  EXPECT_EQ(m.view_prot, static_cast<DWORD>(PAGE_READONLY));
}

TEST(LlvmLibcMemoryPosixMetaTest, MakeRangeRoundtripsAddrAndBytes) {
  void *addr = reinterpret_cast<void *>(uintptr_t(0x10000));
  auto r = mp::make_range(addr, 4096);
  EXPECT_EQ(r.start, addr);
  EXPECT_EQ(r.bytes, size_t(4096));
}
