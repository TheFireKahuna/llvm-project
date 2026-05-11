//===-- Demand-commit VA region for internal subsystems -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Growable VA region backed by reserve-then-commit. Reserves a large range
// upfront (zero physical cost), commits pages as the high-water mark
// advances. Lock-free for concurrent growers.
//
// When to use CommitRegion vs SectionRegion (page_alloc.h):
//   CommitRegion  — linear append-only growth. Userspace CAS watermark
//                   controls commit. Best for arrays that grow from zero
//                   (atexit list, wait-slot pool, remap guard array).
//   SectionRegion — random-access sparse structures. Kernel demand-pages
//                   on first touch, no userspace tracking. Best for hash
//                   tables and lookup arrays with unpredictable access
//                   patterns (mapping table, fd table, pkey state).
//
// Built on page_alloc.h — no mmap, no malloc, no libc dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_COMMIT_REGION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_COMMIT_REGION_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Reserve-then-commit VA region. Trivially constructible (constinit-safe).
//
// Thread-safety: ensure_committed() is lock-free. Multiple threads can
// advance the watermark concurrently — page_commit is idempotent on
// already-committed pages, so redundant commits from racing threads are
// harmless.
class CommitRegion {
  char *base_{nullptr};
  size_t reserved_{0};
  // High-water mark: bytes [0, committed_) are backed by physical pages.
  mutable cpp::Atomic<size_t> committed_{0};

  // Round up to page boundary. Returns 0 on overflow.
  LIBC_INLINE static size_t round_up(size_t n) {
    const size_t mask = windows::get_cached_page_mask();
    if (LIBC_UNLIKELY(n > SIZE_MAX - mask))
      return 0;
    return (n + mask) & ~mask;
  }

public:
  LIBC_INLINE constexpr CommitRegion() = default;

  // Reserve `reserve_bytes` of VA. Optionally commit `initial_commit` bytes.
  // Returns false on failure; safe to retry or abandon.
  [[nodiscard]] LIBC_INLINE bool init(size_t reserve_bytes,
                                      size_t initial_commit = 0) {
    reserve_bytes = round_up(reserve_bytes);
    if (!reserve_bytes)
      return false;
    void *mem = page_reserve(reserve_bytes);
    if (!mem)
      return false;
    base_ = static_cast<char *>(mem);
    reserved_ = reserve_bytes;

    if (initial_commit > 0) {
      initial_commit = round_up(initial_commit);
      if (initial_commit > reserve_bytes)
        initial_commit = reserve_bytes;
      if (!page_commit(base_, initial_commit)) {
        page_free(base_);
        base_ = nullptr;
        reserved_ = 0;
        return false;
      }
      committed_.store(initial_commit, cpp::MemoryOrder::RELEASE);
    }
    return true;
  }

  // Ensure bytes [0, byte_end) are committed. Lock-free, idempotent.
  // Returns false only on page_commit failure (OOM).
  [[nodiscard]] LIBC_INLINE bool ensure_committed(size_t byte_end) {
    size_t cur = committed_.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_LIKELY(byte_end <= cur))
      return true;

    size_t page_end = round_up(byte_end);
    if (LIBC_UNLIKELY(page_end == 0 || page_end > reserved_))
      return false;

    // Re-read watermark — another thread may have advanced it between
    // our first load and here, narrowing or eliminating the commit range.
    cur = committed_.load(cpp::MemoryOrder::ACQUIRE);
    if (cur >= page_end)
      return true;

    // Commit the delta. Idempotent — if another thread races and commits
    // overlapping pages, the redundant commit is a harmless no-op.
    if (LIBC_UNLIKELY(!page_commit(base_ + cur, page_end - cur)))
      return false;

    // CAS-advance watermark. If we lose, someone else advanced further.
    while (cur < page_end) {
      if (committed_.compare_exchange_weak(cur, page_end,
                                           cpp::MemoryOrder::RELEASE,
                                           cpp::MemoryOrder::RELAXED))
        break;
      // cur was reloaded by the CAS — if it's past page_end, we're done.
    }
    return true;
  }

  // Release the entire region. Not thread-safe — call only at shutdown.
  LIBC_INLINE void destroy() {
    if (base_) {
      page_free(base_);
      base_ = nullptr;
    }
    reserved_ = 0;
    committed_.store(0, cpp::MemoryOrder::RELAXED);
  }

  LIBC_INLINE void *base() const { return base_; }
  LIBC_INLINE size_t reserved() const { return reserved_; }
  LIBC_INLINE size_t committed() const {
    return committed_.load(cpp::MemoryOrder::RELAXED);
  }

  // Typed access. Caller must ensure_committed before dereferencing.
  template <typename T> LIBC_INLINE T *as() const {
    return static_cast<T *>(static_cast<void *>(base_));
  }
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_COMMIT_REGION_H
