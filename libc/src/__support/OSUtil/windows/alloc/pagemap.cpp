//===- alloc/pagemap.cpp - Cookie-XOR'd pagemap implementation ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Backs the header's wait-free reader contract with a stay-committed
// PAGE_READONLY reservation (snmalloc lazy-commit; Liétar et al., ISMM
// 2019). The reservation consumes pagefile commit charge (NT enforces
// strict charge with no overcommit) but no physical RAM until pages are
// upgraded RW and written.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/pagemap.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

namespace {

// One atomic byte per pagemap OS page; 0 = still RO-shared-zero, 1 =
// upgraded to RW. 4 MiB for a 16 GiB pagemap reservation covering
// 128 TiB user VA.
//
// Deliberately not sealed in Zone 0: the byte is a syscall-elision hint
// and its worst-case corruption is one redundant or one extra protect()
// call — both harmless. The pagemap itself is protected by the Zone-0
// cookie XOR, which this byte does not cover.
cpp::Atomic<uint8_t> *g_upgrade_state = nullptr;
size_t g_total_pagemap_os_pages = 0;

LIBC_INLINE PagemapEntry *pagemap_base_ptr() {
  return static_cast<PagemapEntry *>(g_pcb.zone0.pagemap_base());
}

LIBC_INLINE void *pagemap_os_page_addr(size_t page_idx) {
  return reinterpret_cast<unsigned char *>(pagemap_base_ptr()) +
         page_idx * kPagemapOsPageSize;
}

struct PagemapPageSpan {
  size_t first_page;
  size_t last_page; // Inclusive.
};

// OS-page span of the pagemap reservation covering every entry for
// [chunk_base, chunk_base + chunk_bytes).
[[nodiscard]] LIBC_INLINE PagemapPageSpan
covering_pagemap_pages(void *chunk_base, size_t chunk_bytes) {
  LIBC_ASSERT(chunk_bytes != 0 && "covering_pagemap_pages: zero bytes");
  uintptr_t chunk_first_idx =
      reinterpret_cast<uintptr_t>(chunk_base) >> kPagemapShift;
  uintptr_t chunk_last_idx =
      (reinterpret_cast<uintptr_t>(chunk_base) + chunk_bytes - 1) >>
      kPagemapShift;
  uintptr_t entry_first_byte = chunk_first_idx * sizeof(PagemapEntry);
  uintptr_t entry_last_byte =
      chunk_last_idx * sizeof(PagemapEntry) + sizeof(PagemapEntry) - 1;

  PagemapPageSpan span;
  span.first_page = entry_first_byte / kPagemapOsPageSize;
  span.last_page = entry_last_byte / kPagemapOsPageSize;
  return span;
}

} // namespace

[[nodiscard]] int pagemap_register_range(void *chunk_base, size_t chunk_bytes) {
  if (LIBC_UNLIKELY(chunk_base == nullptr || chunk_bytes == 0))
    return -EINVAL;
  if (LIBC_UNLIKELY(pagemap_base_ptr() == nullptr))
    __builtin_trap(); // Init must run before any consumer call.
  LIBC_ASSERT((chunk_bytes & (kPagemapChunkBytes - 1)) == 0 &&
              "pagemap_register_range: bytes not chunk-aligned");

  uintptr_t end = reinterpret_cast<uintptr_t>(chunk_base) + chunk_bytes;
  if (LIBC_UNLIKELY(end < reinterpret_cast<uintptr_t>(chunk_base)))
    return -EINVAL;
  PagemapPageSpan span = covering_pagemap_pages(chunk_base, chunk_bytes);
  if (LIBC_UNLIKELY(span.last_page >= g_total_pagemap_os_pages))
    return -EINVAL;

  for (size_t page_idx = span.first_page; page_idx <= span.last_page;
       ++page_idx) {
    // ACQUIRE pairs with the RELEASE store below: load == 1 guarantees
    // the protect() that produced the upgrade has happened-before this
    // load, so the page is safely writable before any pagemap_store.
    if (g_upgrade_state[page_idx].load(cpp::MemoryOrder::ACQUIRE) == 1)
      continue;
    // Concurrent registers on the same page may both call protect(RW)
    // without harm. The byte is RELEASE-stored AFTER protect() returns:
    // a racing reader either still sees 0 (and calls protect again,
    // harmless) or sees 1 (and knows the upgrade has completed).
    void *page_addr = pagemap_os_page_addr(page_idx);
    if (LIBC_UNLIKELY(!::LIBC_NAMESPACE::nt_pal::protect(
            page_addr, kPagemapOsPageSize, PAGE_READWRITE)))
      return -ENOMEM;
    g_upgrade_state[page_idx].store(1, cpp::MemoryOrder::RELEASE);
  }
  return 0;
}

uint32_t pagemap_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                          uint32_t cap) {
  // Size from the user-VA window stamped into Zone 0 by pcb_startup_init.
  // max_address is exclusive; index range is [0, max_address >> kPagemapShift).
  uintptr_t max_va =
      reinterpret_cast<uintptr_t>(g_pcb.zone0.max_address());
  if (LIBC_UNLIKELY(max_va == 0))
    __builtin_trap();
  size_t entries = max_va >> kPagemapShift;
  size_t pagemap_bytes = entries * sizeof(PagemapEntry);
  pagemap_bytes = (pagemap_bytes + kPagemapOsPageSize - 1) &
                  ~(kPagemapOsPageSize - 1);
  size_t total_os_pages = pagemap_bytes / kPagemapOsPageSize;

  // Reserve+commit PAGE_READONLY in one syscall. NT backs every PTE with
  // the kernel shared-zero page; commit charge is consumed (~16 GiB
  // pagefile for a 128 TiB user-VA window), physical RAM is zero until
  // any page is upgraded RW and written. See nt_pal::reserve_commit_readonly
  // — this is the one CI-allowed MEM_COMMIT without MEM_WRITE_WATCH site.
  void *pagemap_addr = nullptr;
  size_t pagemap_actual = pagemap_bytes;
  NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::reserve_commit_readonly(
      &pagemap_addr, &pagemap_actual);
  if (LIBC_UNLIKELY(!NT_SUCCESS(st) || pagemap_addr == nullptr))
    __builtin_trap();

  // Per-OS-page upgrade-state array (private RW, eagerly committed).
  // ~4 MiB at full max-VA reach.
  size_t upgrade_bytes = total_os_pages * sizeof(uint8_t);
  upgrade_bytes = (upgrade_bytes + kPagemapOsPageSize - 1) &
                  ~(kPagemapOsPageSize - 1);
  void *upgrade_addr = nullptr;
  size_t upgrade_actual = upgrade_bytes;
  NTSTATUS up_st = ::LIBC_NAMESPACE::nt_pal::allocate_private(
      &upgrade_addr, &upgrade_actual, PAGE_READWRITE);
  if (LIBC_UNLIKELY(!NT_SUCCESS(up_st)))
    __builtin_trap();

  // Cookie constraint: low byte zero so a zero-filled (or shared-zero)
  // entry decodes with tag = Empty (slot_idx is then `cookie >> 8` —
  // meaningless, callers gate on tag). Acceptance 1/256 so expected
  // ~256 draws. Also reject the all-zero cookie: it would degenerate the
  // XOR to identity, leaving the pagemap unhardened against forged
  // (slot_idx, tag) words. ProcessPrng is the bcryptprimitives entry the
  // rest of the libc uses for sealed cookies.
  uintptr_t cookie = 0;
  for (;;) {
    if (!::ProcessPrng(reinterpret_cast<unsigned char *>(&cookie),
                       sizeof(cookie)))
      __builtin_trap();
    if ((cookie & 0xFFu) == 0 && cookie != 0)
      break;
  }

  g_upgrade_state = static_cast<cpp::Atomic<uint8_t> *>(upgrade_addr);
  g_total_pagemap_os_pages = total_os_pages;
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_pagemap_base(pagemap_addr);
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_pagemap_end(
      static_cast<unsigned char *>(pagemap_addr) + pagemap_bytes);
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_pagemap_cookie(cookie);

  // Two Receipts so Pass 2 stamps both reservations libc-internal.
  if (LIBC_UNLIKELY(cap < 2))
    __builtin_trap();
  out[0].base = pagemap_addr;
  out[0].size = pagemap_bytes;
  out[0].kind = ::LIBC_NAMESPACE::internal::InternalKind::Pagemap;
  out[1].base = upgrade_addr;
  out[1].size = upgrade_bytes;
  out[1].kind = ::LIBC_NAMESPACE::internal::InternalKind::Pagemap;
  return 2;
}

// Intentional no-op. Reservation survives via CoW (shared-zero RO PTEs
// inherit verbatim, touched RW pages CoW on first child write); cookie
// is fork-stable through sealed Zone 0; upgrade-state byte array CoWs
// intact too. Layer-2 consumers (e.g. buddy_arena) reset their own
// per-arena state at their own fork-reinit priority.
void pagemap_fork_reinit() {
}

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
// Section registry hooks
//
// Must expand at namespace scope, hence placement after the closing
// namespace braces above.
//===----------------------------------------------------------------------===//

LIBC_REGISTER_MEMORY_PRIMITIVE(
    pagemap, 3, &::LIBC_NAMESPACE::windows::alloc::pagemap_init_fn)

LIBC_REGISTER_FORK_REINIT(
    pagemap, ::LIBC_NAMESPACE::internal::kForkPrioPagemap,
    &::LIBC_NAMESPACE::windows::alloc::pagemap_fork_reinit)
