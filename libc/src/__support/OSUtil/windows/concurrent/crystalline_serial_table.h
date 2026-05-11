//===-- CrystallineSerialTable — serial → NodeT* radix lookup ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two-level radix table for `BatchLinkCodec` decoders that need an
// allocator-independent NodeT* lookup. Used by Group-B Crystalline
// consumers (ArenaHeader, SlabHeader, ThreadRegistryNode) whose NodeT
// has no naturally-compact 31-bit identity — the consumer stamps a
// monotonic `serial` at allocation, inserts (serial, NodeT*) here,
// and the codec decodes via `lookup(serial)`.
//
// Layout
// ------
//   L1: kL1Slots atomic pointers to L2 pages              (kL1Slots * 8 B BSS)
//   L2: kL2Slots atomic NodeT pointers per page           (kL2Slots * 8 B each,
//                                                          demand-allocated)
//
// At the chosen sizes (kL1Slots=32768, kL2Slots=65536) the table covers
// 2^31 - 1 = 2,147,483,647 serials — same range as the codec encoding
// (bits 0..30; bit 31 reserved for the RNODE tag). Trap on overflow:
// reaching 2^31 serials in process lifetime is unreachable for any
// realistic workload (e.g., a process that allocates one slab every
// nanosecond would take ~70 years to reach the cap), but the trap
// surfaces a stuck monotonic counter or a serial-allocation bug
// loudly rather than silently aliasing.
//
// L2 page allocation uses CAS-publish; loser frees its page back to
// the substrate. L2 pages are never freed for process lifetime —
// once committed, they stay mapped (the table is process-lifetime
// state and shrinking has no benefit at the workload sizes this libc
// targets).
//
// Memory cost
// -----------
// L1 directory: kL1Slots * 8 B = 256 KiB BSS per instantiation.
// L2 pages: kL2Slots * 8 B = 512 KiB each, only committed on first
//   reach into the page's serial range. Workload-bounded.
//
// LA57-safety
// -----------
// The table indexes by serial, not by VA. Encoding does not depend on
// the VA width — works identically on 48-bit and 57-bit (LA57) Windows.
//
// Concurrency
// -----------
// Insert happens once per serial under single-writer discipline (the
// monotonic counter ensures no two inserters target the same slot).
// Lookup is purely-load — no synchronisation between readers and the
// writer beyond the per-pointer ACQUIRE/RELEASE on the L1 and L2
// slots. The L1 ACQUIRE-load synchronises with the inserter's
// L1 CAS-RELEASE (publishes the L2 page and every prior L2 store).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_SERIAL_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_SERIAL_TABLE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

template <typename NodeT> class CrystallineSerialTable {
public:
  // L2 page sized to fit comfortably under one OS allocation:
  // 65536 * 8 B = 512 KiB. Each L1 slot covers 65536 serials.
  static constexpr uint32_t kL2Slots = 65536u;
  // L1 sized to cover the codec's full 31-bit serial range:
  // 32768 * 65536 = 2^31 = 2,147,483,648 serials. The codec reserves
  // bit 31 for RNODE; the maximum encodable serial is 2^31 - 1, so
  // L1[32767] is the last addressable page.
  static constexpr uint32_t kL1Slots = 32768u;
  static constexpr uint32_t kCapacity = kL1Slots * kL2Slots;
  static constexpr size_t kL2PageBytes =
      static_cast<size_t>(kL2Slots) * sizeof(cpp::Atomic<NodeT *>);

  LIBC_INLINE constexpr CrystallineSerialTable() = default;
  CrystallineSerialTable(const CrystallineSerialTable &) = delete;
  CrystallineSerialTable &operator=(const CrystallineSerialTable &) = delete;

  // Publish `node` at `serial`. Single-writer per serial under monotonic
  // counter discipline; the L2-page CAS handles concurrent first-use of
  // adjacent serials in the same page.
  LIBC_INLINE void insert(uint32_t serial, NodeT *node) {
    if (LIBC_UNLIKELY(serial >= kCapacity))
      __builtin_trap();
    uint32_t l1 = serial / kL2Slots;
    uint32_t l2 = serial % kL2Slots;

    cpp::Atomic<NodeT *> *page =
        l1_[l1].load(cpp::MemoryOrder::ACQUIRE);
    if (page == nullptr) {
      void *raw = ::LIBC_NAMESPACE::internal::page_alloc(kL2PageBytes);
      if (LIBC_UNLIKELY(raw == nullptr))
        __builtin_trap();
      __builtin_memset(raw, 0, kL2PageBytes);
      auto *fresh = static_cast<cpp::Atomic<NodeT *> *>(raw);
      cpp::Atomic<NodeT *> *expected = nullptr;
      if (l1_[l1].compare_exchange_strong(expected, fresh,
                                          cpp::MemoryOrder::ACQ_REL,
                                          cpp::MemoryOrder::ACQUIRE)) {
        page = fresh;
      } else {
        ::LIBC_NAMESPACE::internal::page_free(fresh);
        page = expected;
      }
    }
    page[l2].store(node, cpp::MemoryOrder::RELEASE);
  }

  // Resolve `serial` to its NodeT*. Returns nullptr if the serial has
  // never been inserted (decoder bug — should never happen if the
  // codec contract is upheld, but the table itself doesn't trap so
  // callers can validate).
  [[nodiscard]] LIBC_INLINE NodeT *lookup(uint32_t serial) const {
    if (LIBC_UNLIKELY(serial >= kCapacity))
      return nullptr;
    uint32_t l1 = serial / kL2Slots;
    uint32_t l2 = serial % kL2Slots;
    cpp::Atomic<NodeT *> *page =
        l1_[l1].load(cpp::MemoryOrder::ACQUIRE);
    if (page == nullptr)
      return nullptr;
    return page[l2].load(cpp::MemoryOrder::ACQUIRE);
  }

private:
  // L1 directory. Each entry is `cpp::Atomic<cpp::Atomic<NodeT *> *>`
  // — an atomic pointer to a CAS-published L2 page of atomic NodeT*.
  mutable cpp::Atomic<cpp::Atomic<NodeT *> *> l1_[kL1Slots]{};
};

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_SERIAL_TABLE_H
