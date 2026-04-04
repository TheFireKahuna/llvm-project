//===-- Socketpair bidirectional ring buffer channel -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bidirectional connected endpoint pair using the same shared-memory ring
// buffer infrastructure as pipe(). Each endpoint gets a read channel and
// a write channel — two independent FifoChannels cross-wired so that
// endpoint A's write is endpoint B's read and vice versa.
//
// Zero filesystem involvement, zero race conditions, atomic creation.
// Data path: shared-memory ring_copy_in/ring_copy_out — zero kernel calls
// for data transfer (mutant acquire is the only syscall).
//
// Integrates with the OFD discriminated union: ofd->aux.socketpair points
// to the SocketPairChannel (kind == FileKind::SocketPair). Read and write
// channels are accessed via sp->read_ch and sp->write_ch.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKPAIR_CHANNEL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKPAIR_CHANNEL_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Per-endpoint state
//===----------------------------------------------------------------------===//

struct SocketPairChannel {
  FifoChannel *read_ch;   // This endpoint reads from here.
  FifoChannel *write_ch;  // This endpoint writes to here.

  // Shutdown tracking. Bits: SHUT_RD=1, SHUT_WR=2.
  // Atomic — shutdown can race with concurrent I/O.
  cpp::Atomic<uint8_t> shutdown_flags;

  // Domain/type for getsockopt(SO_TYPE) and getsockname().
  int domain;
  int type;
};

inline constexpr uint8_t SP_SHUT_RD = 1;
inline constexpr uint8_t SP_SHUT_WR = 2;

//===----------------------------------------------------------------------===//
// Creation
//===----------------------------------------------------------------------===//

// Create a connected pair of socketpair endpoints. On success, *ep_a and
// *ep_b point to page_alloc'd SocketPairChannels with cross-wired read/write
// channels. On failure, returns false (all resources cleaned up).
LIBC_INLINE bool sp_create_pair(SocketPairChannel **ep_a,
                                 SocketPairChannel **ep_b, int domain,
                                 int type) {
  // Direction 1: A reads, B writes.
  FifoChannel *read_a = nullptr;
  FifoChannel *write_b = nullptr;
  if (!fifo_open_pipe_channel(&read_a, &write_b, 0))
    return false;

  // Direction 2: B reads, A writes.
  FifoChannel *read_b = nullptr;
  FifoChannel *write_a = nullptr;
  if (!fifo_open_pipe_channel(&read_b, &write_a, 0)) {
    fifo_close_channel(read_a);
    fifo_close_channel(write_b);
    return false;
  }

  // Allocate endpoint structs.
  auto *a = static_cast<SocketPairChannel *>(page_alloc(sizeof(SocketPairChannel)));
  auto *b = static_cast<SocketPairChannel *>(page_alloc(sizeof(SocketPairChannel)));
  if (!a || !b) {
    if (a) page_free(a);
    if (b) page_free(b);
    fifo_close_channel(read_a);
    fifo_close_channel(write_b);
    fifo_close_channel(read_b);
    fifo_close_channel(write_a);
    return false;
  }

  // Cross-wire: A writes to B's read ring, B writes to A's read ring.
  a->read_ch = read_a;
  a->write_ch = write_a;
  a->shutdown_flags.store(0, cpp::MemoryOrder::RELAXED);
  a->domain = domain;
  a->type = type;

  b->read_ch = read_b;
  b->write_ch = write_b;
  b->shutdown_flags.store(0, cpp::MemoryOrder::RELAXED);
  b->domain = domain;
  b->type = type;

  *ep_a = a;
  *ep_b = b;
  return true;
}

//===----------------------------------------------------------------------===//
// Shutdown
//===----------------------------------------------------------------------===//

// Shut down one direction. Signals the peer's events so blocked I/O wakes.
LIBC_INLINE void sp_shutdown(SocketPairChannel *ch, int how) {
  uint8_t bits = 0;
  if (how == 0 || how == 2) // SHUT_RD or SHUT_RDWR
    bits |= SP_SHUT_RD;
  if (how == 1 || how == 2) // SHUT_WR or SHUT_RDWR
    bits |= SP_SHUT_WR;

  // fetch_or returns the previous flags; newly_set isolates the bits this
  // call actually transitioned, so each end's refcount is decremented
  // exactly once per real shutdown — fixes a latent double-decrement when
  // sp_shutdown was called twice with the same direction.
  uint8_t old_flags =
      ch->shutdown_flags.fetch_or(bits, cpp::MemoryOrder::ACQ_REL);
  uint8_t newly_set = bits & ~old_flags;

  // Wake peer so it sees EOF (our write shutdown) or EPIPE (our read shutdown).
  if (newly_set & SP_SHUT_WR) {
    // Single LOCK SUB on writer_count; fifo_read sees 0 → EOF.
    ch->write_ch->header()->writer_count.fetch_sub(
        1, cpp::MemoryOrder::ACQ_REL);
    // Wake all futex-parked readers.
    futex_addr::wake(&ch->write_ch->header()->write_pos, UINT32_MAX);
    NtSetEvent(ch->write_ch->readable_event, nullptr);
  }
  if (newly_set & SP_SHUT_RD) {
    // Single LOCK SUB on reader_count; fifo_write sees 0 → EPIPE.
    ch->read_ch->header()->reader_count.fetch_sub(
        1, cpp::MemoryOrder::ACQ_REL);
    // Wake all futex-parked writers.
    futex_addr::wake(&ch->read_ch->header()->read_pos, UINT32_MAX);
    NtSetEvent(ch->read_ch->writable_event, nullptr);
  }
}

//===----------------------------------------------------------------------===//
// Close
//===----------------------------------------------------------------------===//

// Close one endpoint. Closes channels and frees resources.
//
// Does NOT call sp_shutdown — fifo_close_channel already decrements
// the reader/writer refcounts based on open_mode. Calling sp_shutdown
// first would double-decrement, causing premature EOF/EPIPE on the peer.
//
// If shutdown() was previously called (explicit SHUT_RD/SHUT_WR), the
// refcounts were already decremented for those directions. Neutralize
// the corresponding open_mode so fifo_close_channel skips the redundant
// decrement.
LIBC_INLINE void sp_close_channel(SocketPairChannel *ch) {
  uint8_t shut = ch->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);

  // If write was already shut down, writer_count was already decremented
  // by sp_shutdown. Neutralize write_ch->open_mode so fifo_close_channel
  // won't decrement writer_count again. Use -1 as a sentinel that won't
  // match O_RDONLY, O_WRONLY, or O_RDWR.
  if (shut & SP_SHUT_WR)
    ch->write_ch->open_mode = -1;

  // Same for read direction.
  if (shut & SP_SHUT_RD)
    ch->read_ch->open_mode = -1;

  // fifo_close_channel handles refcount decrements and issues futex wakes
  // + event signals on EOF/EPIPE transitions. For neutralized channels
  // (sp_shutdown already called), the refcounts were already decremented
  // and wakes already issued by sp_shutdown.
  fifo_close_channel(ch->read_ch);
  fifo_close_channel(ch->write_ch);
  page_free(ch);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKPAIR_CHANNEL_H
