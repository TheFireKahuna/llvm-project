//===-- Generation-tagged IoRing operation tags ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Encodes a monotonic generation counter and a per-batch slot index into the
// 64-bit UserData field of each IoRing SQE. The generation is echoed back
// in every CQE, enabling two things:
//
//   1. Stale CQE rejection — after EINTR cancellation, leftover CQEs from
//      a previous operation carry an old generation and are silently dropped
//      on the next drain pass. No timeout-based drain heuristics needed.
//
//   2. Batch dispatch — within a single submit, each SQE gets a unique slot
//      index. CQEs arrive in arbitrary completion order; the slot index maps
//      each CQE back to the iovec element (or pipeline stage) that produced
//      it.
//
// Layout:  [generation:32 | slot:32]
//
// The generation counter lives in ThreadRing (per-thread, no contention)
// and wraps at 2^32 — ~4 billion operations per thread before collision.
// Slot index 0 is used for single-op submissions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_OP_TAG_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_OP_TAG_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace ioring {

/// A generation-tagged operation identifier carried in SQE/CQE UserData.
///
/// Constructible from a generation+slot pair, decomposable back into its
/// parts. Value semantics — trivially copyable, zero overhead.
struct OpTag {
  uint32_t generation;
  uint32_t slot;

  /// Encode into the 64-bit UserData field.
  [[nodiscard]] LIBC_INLINE constexpr ULONGLONG as_user_data() const {
    return (static_cast<ULONGLONG>(generation) << 32) |
           static_cast<ULONGLONG>(slot);
  }

  /// Decode from a CQE's UserData field.
  [[nodiscard]] LIBC_INLINE static constexpr OpTag
  from_user_data(ULONGLONG ud) {
    return {static_cast<uint32_t>(ud >> 32), static_cast<uint32_t>(ud)};
  }

  /// Check if this tag belongs to the given generation.
  [[nodiscard]] LIBC_INLINE constexpr bool is_generation(uint32_t gen) const {
    return generation == gen;
  }

  /// Sentinel slot value used for cancel SQEs (never matches a real slot).
  static constexpr uint32_t CANCEL_SLOT = 0xFFFFFFFFu;

  /// Create a cancel tag for the given generation.
  [[nodiscard]] LIBC_INLINE static constexpr OpTag
  make_cancel(uint32_t gen) {
    return {gen, CANCEL_SLOT};
  }

  /// Check if this is a cancel-operation tag.
  [[nodiscard]] LIBC_INLINE constexpr bool is_cancel() const {
    return slot == CANCEL_SLOT;
  }
};

static_assert(sizeof(OpTag) == 8, "OpTag must pack into 8 bytes");

} // namespace ioring
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_OP_TAG_H
