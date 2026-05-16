//===- chunked_append_store.h - Inline + page-block append container -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
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

// Append-only container for trivially-copyable T with a fixed inline buffer
// and a singly-linked chain of page-allocated overflow blocks. Single-
// threaded by contract: every consumer is private state of one skiplist
// transaction owned by exactly one thread, so count / blocks / tail_block
// carry no atomics. Element destructors are never run.
template <typename T, uint32_t kInlineCount, uint32_t kBlockCapacity>
struct ChunkedAppendStore {
    struct Block {
        Block *next{nullptr};
        uint32_t count{0};
        T records[kBlockCapacity]{};
    };

    T inline_data[kInlineCount]{};
    // blocks and tail_block are both null (no overflow ever allocated) or
    // both non-null with tail_block reachable from blocks via next. push()
    // and at() rely on this — neither validates the chain head separately.
    Block *blocks{nullptr};
    Block *tail_block{nullptr};
    uint32_t count{0};

    LIBC_INLINE ChunkedAppendStore() = default;
    ChunkedAppendStore(const ChunkedAppendStore &) = delete;
    ChunkedAppendStore &operator=(const ChunkedAppendStore &) = delete;

    // Shallow pointer transfer of the overflow chain; the moved-from store
    // has blocks / tail_block / count nulled so its destructor's
    // release_storage() walk frees nothing.
    LIBC_INLINE ChunkedAppendStore(ChunkedAppendStore &&other) noexcept
        : blocks(other.blocks), tail_block(other.tail_block),
          count(other.count) {
        __builtin_memcpy(inline_data, other.inline_data, sizeof(inline_data));
        other.blocks = nullptr;
        other.tail_block = nullptr;
        other.count = 0;
    }

    // Deleted: consumers acquire into a pristine slot rather than overwrite
    // a live store, so reassigning a populated store has no defined chain-
    // ownership transfer.
    ChunkedAppendStore &operator=(ChunkedAppendStore &&) = delete;

    LIBC_INLINE ~ChunkedAppendStore() { release_storage(); }

    [[nodiscard]] LIBC_INLINE bool empty() const { return count == 0; }

    // Stricter than empty(): a store that grew past kInlineCount and was
    // then clear()-ed is empty() but not is_unused() — its overflow chain
    // is still held for reuse.
    [[nodiscard]] LIBC_INLINE bool is_unused() const {
        return count == 0 && blocks == nullptr;
    }

    // Returns false on page_alloc failure; caller maps that to -ENOMEM and
    // unwinds the enclosing skiplist transaction via its retire path.
    [[nodiscard]] bool push(const T &value) {
        if (count < kInlineCount) {
            inline_data[count++] = value;
            return true;
        }
        if (tail_block == nullptr) {
            void *mem = ::LIBC_NAMESPACE::internal::page_alloc(sizeof(Block));
            if (mem == nullptr)
                return false;
            __builtin_memset(mem, 0, sizeof(Block));
            tail_block = static_cast<Block *>(mem);
            blocks = tail_block;
        } else if (tail_block->count == kBlockCapacity) {
            if (tail_block->next != nullptr) {
                // Reuse a successor left over from a prior burst + clear().
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
        // count outran the chain: a block's per-block count is inconsistent
        // with the total. Substrate invariant — never a caller bug.
        __builtin_trap();
    }

    [[nodiscard]] const T &at(uint32_t idx) const {
        return const_cast<ChunkedAppendStore *>(this)->at(idx);
    }

    // Retains the overflow chain for reuse; subsequent push() refills the
    // head block first. Inline slots keep stale bytes — count gates reads.
    // Use release_storage() at end-of-life to actually free the chain.
    void clear() {
        count = 0;
        for (Block *block = blocks; block != nullptr; block = block->next)
            block->count = 0;
        tail_block = blocks;
    }

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
