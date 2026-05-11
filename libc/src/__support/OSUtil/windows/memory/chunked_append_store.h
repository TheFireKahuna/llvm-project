//===- chunked_append_store.h - Inline + page-block append container -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Append-only growable container built from a fixed inline array plus a
/// singly-linked chain of page-allocated overflow blocks.
///
/// Used by the va_tracker's transactional builders (\c LockedSet,
/// \c NewNodes) to accumulate records across a single skiplist
/// transaction. The two consumers want different inline / overflow
/// budgets, so the geometry is templated on \c kInlineCount and
/// \c kBlockCapacity.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_CHUNKED_APPEND_STORE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_CHUNKED_APPEND_STORE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

/// Append-only container with a small inline buffer and a chain of
/// page-allocated overflow blocks.
///
/// The container is single-threaded by contract: every consumer
/// (\c LockedSet, \c NewNodes) is private state of a skiplist
/// transaction owned by exactly one thread. There are no atomic
/// operations on \c count, \c blocks, or \c tail_block.
///
/// Layout:
///
/// \code
///     count <= kInlineCount      stored in inline_data[0..count)
///                                blocks == tail_block == nullptr
///                                (no syscall ever issued)
///
///     count >  kInlineCount      inline_data[0..kInlineCount) holds the
///                                first kInlineCount records;
///                                overflow continues in
///                                blocks -> block -> ... -> tail_block,
///                                each carrying its own per-block count
///                                with the final block partially filled
/// \endcode
///
/// Invariants:
///   * \c count is the source of truth for "how many records are live";
///     individual block \c count fields sum (with \c kInlineCount when
///     overflowed) to the same total.
///   * \c blocks and \c tail_block are either both null (no overflow
///     ever allocated) or both non-null with \c tail_block reachable by
///     walking \c next pointers from \c blocks.
///   * After \c clear() the chain is retained for reuse but every block
///     count is zero; the inline array's slots keep their stale bytes
///     and \c count gates every read.
///   * \c push() is the only growth point and is the only call site that
///     may issue \c page_alloc().
///
/// The class deliberately has no copy operations, no move-assignment,
/// and a destructor that frees the entire chain. Move-construction
/// performs a shallow pointer transfer; the moved-from instance is
/// left empty and frees nothing.
///
/// \tparam T              Record type stored in each slot. Must be
///                        trivially copyable and trivially destructible
///                        (no element destructors are run).
/// \tparam kInlineCount   Number of inline slots before the first
///                        overflow block is allocated. Picked to cover
///                        the common-case transaction shape without any
///                        page-allocator traffic.
/// \tparam kBlockCapacity Records per overflow block. Larger values
///                        amortise the per-block \c page_alloc cost but
///                        round physical use up to the next page.
template <typename T, uint32_t kInlineCount, uint32_t kBlockCapacity>
struct ChunkedAppendStore {
    struct Block {
        Block *next{nullptr};
        uint32_t count{0};
        T records[kBlockCapacity]{};
    };

    T inline_data[kInlineCount]{};
    Block *blocks{nullptr};
    Block *tail_block{nullptr};
    uint32_t count{0};

    LIBC_INLINE ChunkedAppendStore() = default;
    ChunkedAppendStore(const ChunkedAppendStore &) = delete;
    ChunkedAppendStore &operator=(const ChunkedAppendStore &) = delete;

    LIBC_INLINE ChunkedAppendStore(ChunkedAppendStore &&other) noexcept
        : blocks(other.blocks), tail_block(other.tail_block),
          count(other.count) {
        __builtin_memcpy(inline_data, other.inline_data, sizeof(inline_data));
        other.blocks = nullptr;
        other.tail_block = nullptr;
        other.count = 0;
    }

    // Move-assignment is deleted: consumers acquire into an existing
    // pristine slot (LockedSet::acquire) rather than overwriting a live
    // store. Reassigning a populated store has no sensible semantics
    // for the per-transaction lifecycle.
    ChunkedAppendStore &operator=(ChunkedAppendStore &&) = delete;

    LIBC_INLINE ~ChunkedAppendStore() { release_storage(); }

    [[nodiscard]] LIBC_INLINE bool empty() const { return count == 0; }

    /// Returns true iff this store has never been pushed into.
    ///
    /// Stricter than \c empty(): a store that grew past \c kInlineCount
    /// and was then \c clear()-ed retains its overflow chain for reuse
    /// and returns \c empty() but not \c is_unused().
    [[nodiscard]] LIBC_INLINE bool is_unused() const {
        return count == 0 && blocks == nullptr;
    }

    /// Appends \p value at logical index \c count.
    ///
    /// Allocates a fresh overflow \c Block via \c page_alloc on the
    /// transitions \c count == kInlineCount (first overflow) and on
    /// every \c kBlockCapacity boundary where the existing chain has no
    /// pre-allocated successor. Blocks previously allocated and then
    /// released back via \c clear() are reused without re-allocation.
    ///
    /// \returns true on success; false on \c page_alloc failure. The
    ///          caller treats false as \c -ENOMEM and unwinds the
    ///          enclosing skiplist transaction via its retire path.
    [[nodiscard]] bool push(const T &value) {
        if (count < kInlineCount) {
            inline_data[count++] = value;
            return true;
        }
        if (tail_block == nullptr) {
            // First overflow ever: allocate the head block. blocks and
            // tail_block transition from null to the new block together.
            void *mem = ::LIBC_NAMESPACE::internal::page_alloc(sizeof(Block));
            if (mem == nullptr)
                return false;
            __builtin_memset(mem, 0, sizeof(Block));
            tail_block = static_cast<Block *>(mem);
            blocks = tail_block;
        } else if (tail_block->count == kBlockCapacity) {
            if (tail_block->next != nullptr) {
                // Reuse a previously-allocated successor left over from
                // an earlier push burst followed by clear().
                tail_block = tail_block->next;
            } else {
                void *mem =
                    ::LIBC_NAMESPACE::internal::page_alloc(sizeof(Block));
                if (mem == nullptr)
                    return false;
                __builtin_memset(mem, 0, sizeof(Block));
                Block *block = static_cast<Block *>(mem);
                tail_block->next = block;
                tail_block = block;
            }
        }
        tail_block->records[tail_block->count++] = value;
        ++count;
        return true;
    }

    /// Returns a reference to the record at logical index \p idx.
    ///
    /// \pre \p idx < \c count.
    ///
    /// Indices in \c [0, kInlineCount) read from \c inline_data; higher
    /// indices walk the overflow chain decrementing \p idx by each
    /// block's local count. A mismatch between \c count and the chain
    /// (the chain ran out before \p idx did) traps as a substrate
    /// invariant violation.
    [[nodiscard]] T &at(uint32_t idx) {
        LIBC_ASSERT(idx < count);
        if (idx < kInlineCount)
            return inline_data[idx];
        idx -= kInlineCount;
        for (Block *block = blocks; block != nullptr; block = block->next) {
            if (idx < block->count)
                return block->records[idx];
            idx -= block->count;
        }
        __builtin_trap();
    }

    [[nodiscard]] const T &at(uint32_t idx) const {
        return const_cast<ChunkedAppendStore *>(this)->at(idx);
    }

    /// Resets the live count to zero while retaining the overflow
    /// chain.
    ///
    /// Each block's per-block count is also zeroed so subsequent
    /// \c push() reuses the head block first and walks forward as it
    /// fills. The inline array's slots keep their stale bytes; \c count
    /// is the only thing that bounds a subsequent read.
    ///
    /// Use \c release_storage() instead when the consumer is sure the
    /// store will not be reused — e.g. on transaction commit/abort if
    /// the LockedSet is about to be destroyed anyway.
    void clear() {
        count = 0;
        for (Block *block = blocks; block != nullptr; block = block->next)
            block->count = 0;
        tail_block = blocks;
    }

    /// Frees every overflow block via \c page_free and resets the
    /// store to its as-default-constructed shape.
    void release_storage() {
        Block *block = blocks;
        while (block != nullptr) {
            Block *next = block->next;
            ::LIBC_NAMESPACE::internal::page_free(block);
            block = next;
        }
        blocks = nullptr;
        tail_block = nullptr;
        count = 0;
    }
};

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_CHUNKED_APPEND_STORE_H
