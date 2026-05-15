//===- posix_mutators.cpp - DescMutator callbacks for POSIX ops -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Twelve free-function `DescMutator` callbacks for the POSIX layer.
// Each mutator runs on a fresh `RegionDesc` clone under the substrate's
// LOCKED hold and writes only the fields its intent names; unrelated
// `flags` bits are preserved through the OR / AND-NOT pattern so a
// later mprotect after MADV_DONTFORK keeps the DONTFORK bit intact.
//
// Atomic ordering: `flags` is `cpp::Atomic<uint16_t>`. Writes use
// `RELEASE` so a future reader pinning the clone observes the mutation;
// reads use `RELAXED` because the clone has no concurrent writer and
// `RELEASE` on store is sufficient for cross-thread publication.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/posix_mutators.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory_posix {

namespace {

using ::LIBC_NAMESPACE::cpp::MemoryOrder;
using ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc;

// Set the flag bits in `bits` on `d->flags`, preserving every other bit.
LIBC_INLINE void set_flag_bits(RegionDesc *d, uint16_t bits) {
  uint16_t cur = d->flags.load(MemoryOrder::RELAXED);
  d->flags.store(static_cast<uint16_t>(cur | bits), MemoryOrder::RELEASE);
}

// Clear the flag bits in `bits` on `d->flags`, preserving every other bit.
LIBC_INLINE void clear_flag_bits(RegionDesc *d, uint16_t bits) {
  uint16_t cur = d->flags.load(MemoryOrder::RELAXED);
  d->flags.store(static_cast<uint16_t>(cur & ~bits), MemoryOrder::RELEASE);
}

} // namespace

void lock_mutator(RegionDesc *new_desc, void *ctx) {
  (void)new_desc;
  (void)ctx;
}

void lock_set_onfault_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  set_flag_bits(new_desc,
                ::LIBC_NAMESPACE::windows::va_tracker::region_flag::LOCK_ONFAULT);
}

void lock_clear_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  clear_flag_bits(new_desc,
                  ::LIBC_NAMESPACE::windows::va_tracker::region_flag::LOCK_ONFAULT);
}

void brk_extend_mutator(RegionDesc *new_desc, void *ctx) {
  auto *c = static_cast<BrkExtendCtx *>(ctx);
  new_desc->section_offset.QuadPart =
      static_cast<LONGLONG>(reinterpret_cast<uintptr_t>(c->new_cursor));
}

void numa_rebind_mutator(RegionDesc *new_desc, void *ctx) {
  auto *c = static_cast<NumaRebindCtx *>(ctx);
  new_desc->numa_interleave_mask = c->nodemask;
  using namespace ::LIBC_NAMESPACE::windows::va_tracker::region_flag;
  if (c->interleave)
    set_flag_bits(new_desc, NUMA_INTERLEAVE);
  else
    clear_flag_bits(new_desc, NUMA_INTERLEAVE);
}

void dump_set_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  set_flag_bits(new_desc,
                ::LIBC_NAMESPACE::windows::va_tracker::region_flag::DUMP_EXCLUDE);
}

void dump_clear_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  clear_flag_bits(new_desc,
                  ::LIBC_NAMESPACE::windows::va_tracker::region_flag::DUMP_EXCLUDE);
}

void guard_set_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  set_flag_bits(new_desc,
                ::LIBC_NAMESPACE::windows::va_tracker::region_flag::PROT_GUARD);
}

void guard_clear_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  clear_flag_bits(new_desc,
                  ::LIBC_NAMESPACE::windows::va_tracker::region_flag::PROT_GUARD);
}

void fork_set_dontfork_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  set_flag_bits(new_desc,
                ::LIBC_NAMESPACE::windows::va_tracker::region_flag::DONTFORK);
}

void fork_clear_dontfork_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  clear_flag_bits(new_desc,
                  ::LIBC_NAMESPACE::windows::va_tracker::region_flag::DONTFORK);
}

void fork_set_wipeonfork_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  set_flag_bits(new_desc,
                ::LIBC_NAMESPACE::windows::va_tracker::region_flag::WIPEONFORK);
}

void fork_clear_wipeonfork_mutator(RegionDesc *new_desc, void *ctx) {
  (void)ctx;
  clear_flag_bits(new_desc,
                  ::LIBC_NAMESPACE::windows::va_tracker::region_flag::WIPEONFORK);
}

} // namespace memory_posix
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
