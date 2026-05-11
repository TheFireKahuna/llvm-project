//===-- Epoll state for reactor-backed event notification ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// epoll on Windows: the process-wide reactor IOCP as the backbone.
//
//   AFD sockets:     IOCTL_AFD_NOTIFY -> completions to reactor::iocp_handle().
//   Socketpair/FIFO: WCP bridges events to reactor::iocp_handle().
//   Regular files:   Synthetic immediate completion via NtSetIoCompletionEx.
//
// The reactor's drain thread routes non-reactor completions to per-instance
// pending queues via the installed CompletionRouter. epoll_wait blocks on
// a per-instance ready_event, then drains the pending queue.
//
// Before blocking, epoll_wait does a non-blocking IOCP flush to pick up
// freshly-arrived completions that the drain thread hasn't routed yet.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_EPOLL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_EPOLL_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/threads/raw_mutex.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Forward declaration
//===----------------------------------------------------------------------===//

struct EpollInstance;

//===----------------------------------------------------------------------===//
// Per-fd registration within an epoll instance
//===----------------------------------------------------------------------===//

struct EpollRegistration {
  int fd;
  uint32_t events;          // EPOLLIN | EPOLLOUT | EPOLLET | ...
  uint64_t data;            // epoll_data_t.u64 (user data)
  uint32_t generation;      // Matches FdSlot -- detects stale registrations.
  bool is_afd;              // true: AFD_NOTIFY, false: WCP bridge.
  bool is_pipe;             // true: raw NT pipe, sentinel-read notification.
  bool pipe_et_suppressed;  // EPOLLET: event already delivered, suppress until
                            // pipe drains and new data arrives.

  // WCP handles for non-AFD fds (socketpair, FIFO).
  HANDLE wcp_in;            // Bridges readable_event -> IOCP.
  HANDLE wcp_out;           // Bridges writable_event -> IOCP.

  // Sentinel read for pipe fds. A 1-byte async NtReadFile on a duplicate
  // handle (byte-stream mode). When data arrives, the read completes and
  // consumes 1 byte into sentinel_buf. That byte is pushed to the OFD's
  // pipe_lookahead for the read path to prepend. Standard IOCP pattern.
  HANDLE sentinel_handle;        // Duplicate handle (byte-stream mode).
  IO_STATUS_BLOCK sentinel_iosb; // IOSB for the pending sentinel read.

  // Back-pointer to owning instance. Atomic for safe concurrent access:
  // the close path stores nullptr (RELEASE) to tombstone the registration;
  // the completion router loads (ACQUIRE) and bails if null. This prevents
  // the router from chasing a dangling pointer into a freed instance.
  cpp::Atomic<EpollInstance *> instance;
};

//===----------------------------------------------------------------------===//
// Pending completion entry -- routed by the drain thread
//===----------------------------------------------------------------------===//

struct EpollPendingEntry {
  PVOID key;                // CompletionKey (EpollRegistration pointer).
  NTSTATUS status;          // IoStatusBlock.Status.
  ULONG_PTR information;    // IoStatusBlock.Information.
};

//===----------------------------------------------------------------------===//
// Epoll instance -- one per epoll_create()
//===----------------------------------------------------------------------===//

// Pending queue capacity. Sized generously -- the drain thread pushes at
// most DRAIN_BATCH_SIZE (32) per iteration, and epoll_wait drains the
// queue on every call. Overflow is extremely unlikely.
inline constexpr uint32_t EPOLL_PENDING_CAPACITY = 512;

inline constexpr uint32_t EPOLL_INITIAL_CAPACITY = 64;

struct EpollInstance {
  // Per-instance ready event (manual-reset / NotificationEvent).
  // Signaled by the drain thread when pending entries exist.
  // Cleared by epoll_wait when the queue is drained to empty.
  HANDLE ready_event;

  // Per-instance reserve for guaranteed-delivery posts (synthetic
  // completions for regular files, manual wakeups).
  HANDLE reserve;

  RawMutex lock;            // Protects registration table modifications.

  EpollRegistration *regs;  // Open-addressing hash table (fd -> reg).
  uint32_t capacity;        // Table size (power of 2).
  uint32_t count;           // Active registrations.

  // Pending completion queue -- ring buffer written by the drain thread,
  // read by epoll_wait. Protected by pending_lock.
  RawMutex pending_lock;
  EpollPendingEntry *pending; // Ring buffer (EPOLL_PENDING_CAPACITY entries).
  uint32_t pending_head;      // Consumer index (epoll_wait).
  uint32_t pending_tail;      // Producer index (drain thread).

  // Drain sequence counter — incremented by epoll_wait after consuming
  // entries from the pending queue. The completion router parks on this
  // via futex when the pending queue is full, avoiding busy-spin.
  cpp::Atomic<uint32_t> drain_seq{0};
};

//===----------------------------------------------------------------------===//
// Pending queue operations
//===----------------------------------------------------------------------===//

// Push a completion into the instance's pending queue. Called by the
// reactor's completion router on the drain thread. Returns true if
// pushed successfully, false if the queue is full.
LIBC_INLINE bool epoll_push_pending(EpollInstance *inst, PVOID key,
                                     NTSTATUS status, ULONG_PTR information) {
  inst->pending_lock.lock();
  uint32_t next_tail = (inst->pending_tail + 1) % EPOLL_PENDING_CAPACITY;
  if (next_tail == inst->pending_head) {
    inst->pending_lock.unlock();
    return false; // Queue full.
  }
  inst->pending[inst->pending_tail] = {key, status, information};
  inst->pending_tail = next_tail;
  inst->pending_lock.unlock();
  return true;
}

// Drain pending entries into an epoll_event array. Called by epoll_wait.
// Returns the number of events written. Does NOT clear ready_event --
// the caller handles that after checking if the queue is empty.
LIBC_INLINE uint32_t epoll_pending_count(EpollInstance *inst) {
  inst->pending_lock.lock();
  uint32_t count = (inst->pending_tail - inst->pending_head) %
                   EPOLL_PENDING_CAPACITY;
  inst->pending_lock.unlock();
  return count;
}

//===----------------------------------------------------------------------===//
// Registration helpers (must precede allocation — used by alloc/remove)
//===----------------------------------------------------------------------===//

// Copy all plain fields from src to dst. The atomic `instance` field
// is loaded/stored explicitly because EpollRegistration is non-copyable
// due to the cpp::Atomic member.
LIBC_INLINE void epoll_reg_copy(EpollRegistration *dst,
                                EpollRegistration *src) {
  dst->fd = src->fd;
  dst->events = src->events;
  dst->data = src->data;
  dst->generation = src->generation;
  dst->is_afd = src->is_afd;
  dst->is_pipe = src->is_pipe;
  dst->pipe_et_suppressed = src->pipe_et_suppressed;
  dst->wcp_in = src->wcp_in;
  dst->wcp_out = src->wcp_out;
  dst->sentinel_handle = src->sentinel_handle;
  dst->sentinel_iosb = src->sentinel_iosb;
  dst->instance.store(src->instance.load(cpp::MemoryOrder::RELAXED),
                      cpp::MemoryOrder::RELAXED);
}

LIBC_INLINE void epoll_reg_clear(EpollRegistration *reg) {
  reg->fd = -1;
  reg->events = 0;
  reg->data = 0;
  reg->generation = 0;
  reg->is_afd = false;
  reg->is_pipe = false;
  reg->pipe_et_suppressed = false;
  reg->wcp_in = nullptr;
  reg->wcp_out = nullptr;
  reg->sentinel_handle = nullptr;
  reg->sentinel_iosb = {};
  reg->instance.store(nullptr, cpp::MemoryOrder::RELAXED);
}

//===----------------------------------------------------------------------===//
// Allocation
//===----------------------------------------------------------------------===//

LIBC_INLINE EpollInstance *epoll_instance_alloc() {
  auto *inst =
      static_cast<EpollInstance *>(page_alloc(sizeof(EpollInstance)));
  if (!inst)
    return nullptr;
  // page_alloc returns zeroed memory (committed pages). Zero-init is
  // correct for HANDLE (nullptr), RawMutex, uint32_t, and pointer fields.
  __builtin_memset(inst, 0, sizeof(EpollInstance));
  // Atomic members need explicit initialization after memset.
  inst->drain_seq.store(0, cpp::MemoryOrder::RELAXED);

  // Allocate the registration table.
  size_t table_bytes = EPOLL_INITIAL_CAPACITY * sizeof(EpollRegistration);
  inst->regs =
      static_cast<EpollRegistration *>(page_alloc(table_bytes));
  if (!inst->regs) {
    page_free(inst);
    return nullptr;
  }

  // Initialize each registration slot field-by-field. EpollRegistration
  // contains an Atomic<> member (instance), so memset is not safe —
  // use epoll_reg_clear which handles the atomic store properly.
  for (uint32_t i = 0; i < EPOLL_INITIAL_CAPACITY; ++i)
    epoll_reg_clear(&inst->regs[i]);

  // Allocate the pending queue.
  size_t pending_bytes = EPOLL_PENDING_CAPACITY * sizeof(EpollPendingEntry);
  inst->pending =
      static_cast<EpollPendingEntry *>(page_alloc(pending_bytes));
  if (!inst->pending) {
    page_free(inst->regs);
    page_free(inst);
    return nullptr;
  }
  __builtin_memset(inst->pending, 0, pending_bytes);

  inst->capacity = EPOLL_INITIAL_CAPACITY;
  return inst;
}

// Phase 1: Cancel all outstanding I/O and tombstone all registrations.
// After this returns, any completion router invocation for this instance
// will see instance==nullptr and bail without touching the instance.
// Does NOT free any pages — the caller must fence, then call free_pages.
LIBC_INLINE void epoll_instance_shutdown(EpollInstance *inst) {
  if (!inst)
    return;

  for (uint32_t i = 0; i < inst->capacity; ++i) {
    if (inst->regs[i].fd >= 0) {
      // Cancel and close WCP handles.
      if (inst->regs[i].wcp_in) {
        NtCancelWaitCompletionPacket(inst->regs[i].wcp_in, TRUE);
        NtClose(inst->regs[i].wcp_in);
      }
      if (inst->regs[i].wcp_out) {
        NtCancelWaitCompletionPacket(inst->regs[i].wcp_out, TRUE);
        NtClose(inst->regs[i].wcp_out);
      }
      // Cancel sentinel reads and wait for the IOSB to leave PENDING,
      // ensuring the cancel completion has been posted to the IOCP.
      if (inst->regs[i].sentinel_handle) {
        IO_STATUS_BLOCK cancel_iosb = {};
        NtCancelIoFileEx(inst->regs[i].sentinel_handle,
                         &inst->regs[i].sentinel_iosb, &cancel_iosb);
        // Wait for the kernel to complete the cancellation. No explicit
        // waker — the kernel writes to IOSB.Status directly. wait_nt's
        // internal 32-iteration spin catches fast completions; the
        // timeout handles slow ones.
        {
          LARGE_INTEGER cancel_timeout;
          cancel_timeout.QuadPart = -100000; // 10ms per iteration
          while (inst->regs[i].sentinel_iosb.Status ==
                 static_cast<NTSTATUS>(STATUS_PENDING))
            futex_addr::wait_nt<int32_t>(
                reinterpret_cast<const volatile int32_t *>(
                    &inst->regs[i].sentinel_iosb.Status),
                static_cast<int32_t>(STATUS_PENDING), &cancel_timeout);
        }
        NtClose(inst->regs[i].sentinel_handle);
      }

      // Tombstone: atomically null the instance pointer. Any concurrent
      // router invocation that loads this (ACQUIRE) will see nullptr and
      // bail without touching the instance. RELEASE ordering ensures all
      // cancellation above is visible before the tombstone.
      inst->regs[i].instance.store(nullptr, cpp::MemoryOrder::RELEASE);
      inst->regs[i].fd = -1;
    }
  }
}

// Phase 2: Free all backing pages. Only safe after fencing (no thread
// holds a pointer into regs[] or inst).
LIBC_INLINE void epoll_instance_free_pages(EpollInstance *inst) {
  if (!inst)
    return;

  if (inst->pending)
    page_free(inst->pending);
  if (inst->regs)
    page_free(inst->regs);
  if (inst->reserve)
    NtClose(inst->reserve);
  if (inst->ready_event)
    NtClose(inst->ready_event);
  page_free(inst);
}

// Convenience: shutdown + immediate free (for use when no in-flight
// completions are possible, e.g., epoll_create failure rollback).
LIBC_INLINE void epoll_instance_free(EpollInstance *inst) {
  epoll_instance_shutdown(inst);
  epoll_instance_free_pages(inst);
}

//===----------------------------------------------------------------------===//
// Hash table operations (open addressing, linear probe)
//===----------------------------------------------------------------------===//

LIBC_INLINE EpollRegistration *epoll_find(EpollInstance *inst, int fd) {
  uint32_t idx = static_cast<uint32_t>(fd) & (inst->capacity - 1);
  for (uint32_t probe = 0; probe < inst->capacity; ++probe) {
    uint32_t slot = (idx + probe) & (inst->capacity - 1);
    if (inst->regs[slot].fd == fd)
      return &inst->regs[slot];
    if (inst->regs[slot].fd == -1)
      return nullptr; // Empty slot -- fd not found.
  }
  return nullptr;
}

LIBC_INLINE EpollRegistration *epoll_insert(EpollInstance *inst, int fd) {
  uint32_t idx = static_cast<uint32_t>(fd) & (inst->capacity - 1);
  for (uint32_t probe = 0; probe < inst->capacity; ++probe) {
    uint32_t slot = (idx + probe) & (inst->capacity - 1);
    if (inst->regs[slot].fd == -1) {
      inst->regs[slot].fd = fd;
      ++inst->count;
      return &inst->regs[slot];
    }
  }
  return nullptr; // Table full.
}

LIBC_INLINE void epoll_remove(EpollInstance *inst, int fd) {
  EpollRegistration *reg = epoll_find(inst, fd);
  if (!reg)
    return;

  // Close WCP handles.
  if (reg->wcp_in) {
    NtCancelWaitCompletionPacket(reg->wcp_in, TRUE);
    NtClose(reg->wcp_in);
  }
  if (reg->wcp_out) {
    NtCancelWaitCompletionPacket(reg->wcp_out, TRUE);
    NtClose(reg->wcp_out);
  }
  // Cancel sentinel read and wait for the IOSB to leave PENDING before
  // clearing the slot. Without this wait, the kernel may write to
  // sentinel_iosb.Status after epoll_reg_clear zeroes the slot, corrupting
  // a recycled registration.
  if (reg->sentinel_handle) {
    IO_STATUS_BLOCK cancel_iosb = {};
    NtCancelIoFileEx(reg->sentinel_handle, &reg->sentinel_iosb, &cancel_iosb);
    // Wait for the kernel to complete the cancellation. wait_nt's
    // internal 32-iteration spin catches fast completions; the timeout
    // handles slow ones.
    {
      LARGE_INTEGER cancel_timeout;
      cancel_timeout.QuadPart = -100000; // 10ms per iteration
      while (reg->sentinel_iosb.Status ==
             static_cast<NTSTATUS>(STATUS_PENDING))
        futex_addr::wait_nt<int32_t>(
            reinterpret_cast<const volatile int32_t *>(
                &reg->sentinel_iosb.Status),
            static_cast<int32_t>(STATUS_PENDING), &cancel_timeout);
    }
    NtClose(reg->sentinel_handle);
  }

  epoll_reg_clear(reg);
  --inst->count;

  // Rehash subsequent entries to maintain probe chain integrity.
  uint32_t slot =
      static_cast<uint32_t>(reg - inst->regs);
  for (uint32_t probe = 1; probe < inst->capacity; ++probe) {
    uint32_t next = (slot + probe) & (inst->capacity - 1);
    if (inst->regs[next].fd == -1)
      break; // End of chain.
    uint32_t ideal =
        static_cast<uint32_t>(inst->regs[next].fd) & (inst->capacity - 1);
    // If this entry's ideal slot is at or before the gap, move it.
    if ((next > slot && (ideal <= slot || ideal > next)) ||
        (next < slot && ideal <= slot && ideal > next)) {
      epoll_reg_copy(&inst->regs[slot], &inst->regs[next]);
      inst->regs[next].fd = -1;
      inst->regs[next].instance.store(nullptr, cpp::MemoryOrder::RELAXED);
      slot = next;
    }
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SOCKET_EPOLL_H
