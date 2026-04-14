//===-- NT-native IoRing operations -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Direct NT IoRing operations replacing kernelbase.dll wrappers.
//
// SQE push and CQE pop are pure userspace shared memory operations — no
// syscall. Only submit() crosses into the kernel (NtSubmitIoRing).
//
// RingState is designed to embed directly in OpenFileDescription (56 bytes,
// no heap allocation). It holds the NT handle plus the minimum shared-memory
// pointers and masks needed for lock-free SQE/CQE access.
//
// Both the SQ and CQ sides use local index caching to avoid reading the
// remote party's index from shared memory on the fast path. The kernel-mapped
// SQ/CQ headers pack Head and Tail on the same cache line, so every cross-side
// read is a cache-line transfer. The caches (sq_head_cache, cq_tail_cache)
// are only refreshed when the ring appears full (SQ) or empty (CQ), reducing
// cross-core traffic to roughly once per ring capacity instead of once per op.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_OPS_H

#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"   // NtLock/UnlockVirtualMemory
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h" // MAP_PROCESS
#include "src/__support/OSUtil/windows/nt/nt_process.h"      // NtClose
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace ioring {

//===----------------------------------------------------------------------===//
// RingState — embeddable in OpenFileDescription (56 bytes)
//===----------------------------------------------------------------------===//

struct RingState {
  HANDLE nt_handle;      // Kernel IoRing handle
  NT_IORING_SQ *sq;      // Mapped submission queue
  NT_IORING_CQ *cq;      // Mapped completion queue
  ULONG sq_mask;         // SQ index mask (sq_size - 1)
  ULONG cq_mask;         // CQ index mask
  ULONG sq_tail;         // User-side SQ producer index
  ULONG sq_head_cache;   // Cached SQ.Head — re-read from shared memory
                         // only when ring appears full. Saves an atomic
                         // load on every acquire_sqe.
  ULONG cq_head;         // User-side CQ consumer index — avoids reading
                         // our own Head back from the kernel-shared page
                         // (which may be cache-invalidated by kernel Tail
                         // writes on the same cache line).
  ULONG cq_head_published; // Last Head value written to shared memory.
                         // CQ Head release-store is deferred until the
                         // next submit() — no point telling the kernel
                         // about reclaimed CQ slots until we actually
                         // need it to process new SQEs.
  ULONG cq_tail_cache;   // Cached CQ.Tail — re-read from shared memory
                         // only when ring appears empty. Mirrors the
                         // sq_head_cache pattern for the consumer side.
  ULONG needs_wake;      // Tri-state flag read by reactor waker callback:
                         //   0 = no thread waiting (skip wake)
                         //   1 = thread parked on futex (futex_addr::wake)
                         //   2 = thread in alertable NtWaitForSingleObject
                         //       (signal wait_event)
                         // Cross-thread access uses __atomic.
  DWORD owner_tid;       // Thread ID of the owning I/O thread, read by
                         // the reactor waker callback to call
                         // NtAlertThreadByThreadId directly (bypasses
                         // the parking lot).
};

// 3 pointers (24) + 9 ULONGs (36) + 1 DWORD (4) = 64.
// Fits exactly in one 64-byte cache line.
static_assert(sizeof(RingState) <= 64, "RingState layout changed");

//===----------------------------------------------------------------------===//
// Lifecycle
//===----------------------------------------------------------------------===//

// Create an IoRing. Fills ring from NtCreateIoRing output.
LIBC_INLINE NTSTATUS create(RingState *ring, ULONG version, ULONG sq_size,
                            ULONG cq_size) {
  NT_IORING_STRUCTV1 params = {};
  params.IoRingVersion = version;
  params.SubmissionQueueSize = sq_size;
  params.CompletionQueueSize = cq_size;

  NT_IORING_INFO info = {};
  NTSTATUS s = NtCreateIoRing(&ring->nt_handle, sizeof(params), &params,
                              sizeof(info), &info);
  if (!NT_SUCCESS(s)) {
    ring->nt_handle = nullptr;
    return s;
  }

  // Extract the fields we need for runtime operation.
  ring->sq = info.SubmissionQueue;
  ring->cq = info.CompletionQueue;
  ring->sq_mask = info.SubmissionQueueRingMask;
  ring->cq_mask = info.CompletionQueueRingMask;
  ring->sq_tail = 0;
  ring->sq_head_cache = 0;
  ring->cq_head = 0;
  ring->cq_head_published = 0;
  ring->cq_tail_cache = 0;
  ring->needs_wake = 0;
  ring->owner_tid = 0; // Set by thread_ring_create after ring init.

  // Pin SQ/CQ shared memory in the working set. These pages are touched
  // on every I/O op; locking prevents soft faults under memory pressure.
  // Best-effort — failure just means normal demand-paging behavior.
  {
    PVOID sq_base = ring->sq;
    SIZE_T sq_len = sizeof(NT_IORING_SQ) +
                    ring->sq_mask * sizeof(NT_IORING_SQE);
    ::NtLockVirtualMemory(NtCurrentProcess(), &sq_base, &sq_len, MAP_PROCESS);

    PVOID cq_base = ring->cq;
    SIZE_T cq_len = sizeof(NT_IORING_CQ) +
                    (ring->cq_mask) * sizeof(NT_IORING_CQE);
    ::NtLockVirtualMemory(NtCurrentProcess(), &cq_base, &cq_len, MAP_PROCESS);
  }

  return STATUS_SUCCESS;
}

// Close an IoRing. Unlocks SQ/CQ shared memory before closing the handle,
// since NtClose unmaps the kernel-mapped pages.
LIBC_INLINE void close(RingState *ring) {
  if (ring->nt_handle) {
    // Unlock before NtClose — the handle close unmaps the shared memory,
    // so unlocking after would operate on unmapped VA (harmless NTSTATUS
    // error, but wasteful). Best-effort: ignore failures.
    if (ring->sq) {
      PVOID sq_base = ring->sq;
      SIZE_T sq_len = sizeof(NT_IORING_SQ) +
                      ring->sq_mask * sizeof(NT_IORING_SQE);
      ::NtUnlockVirtualMemory(NtCurrentProcess(), &sq_base, &sq_len,
                              MAP_PROCESS);
    }
    if (ring->cq) {
      PVOID cq_base = ring->cq;
      SIZE_T cq_len = sizeof(NT_IORING_CQ) +
                      (ring->cq_mask) * sizeof(NT_IORING_CQE);
      ::NtUnlockVirtualMemory(NtCurrentProcess(), &cq_base, &cq_len,
                              MAP_PROCESS);
    }
    NtClose(ring->nt_handle);
    ring->nt_handle = nullptr;
  }
}

// Register a completion event (empty→non-empty CQ transition).
LIBC_INLINE NTSTATUS set_completion_event(RingState *ring, HANDLE event) {
  NT_IORING_COMPLETION_EVENT_INFO evi = {event};
  return NtSetInformationIoRing(ring->nt_handle,
                                IoRingInformationClassCompletionEvent,
                                sizeof(evi), &evi);
}

//===----------------------------------------------------------------------===//
// SQE push — pure userspace
//===----------------------------------------------------------------------===//

// Acquire an SQE slot. Returns nullptr if the SQ is full.
// The slot is NOT zeroed — callers must set all fields relevant to their
// opcode. For read/write ops, push_read/push_write handle this explicitly,
// saving ~50-100 cycles vs a 64-byte memset per SQE.
//
// Uses cached SQ.Head — only re-reads from shared memory when the ring
// appears full. This saves one atomic load per SQE push on the fast path.
LIBC_INLINE NT_IORING_SQE *acquire_sqe(RingState *ring) {
  ULONG tail = ring->sq_tail;

  if (tail - ring->sq_head_cache > ring->sq_mask) {
    // Ring appears full — re-read head from shared memory.
    ring->sq_head_cache = __atomic_load_n(&ring->sq->Head, __ATOMIC_ACQUIRE);
    if (tail - ring->sq_head_cache > ring->sq_mask)
      return nullptr; // Actually full.
  }

  NT_IORING_SQE *sqe = &ring->sq->Entries[tail & ring->sq_mask];
  ring->sq_tail = tail + 1;
  return sqe;
}

// Return how many SQE slots the kernel has consumed since last check.
// Useful for back-pressure monitoring.
LIBC_INLINE ULONG sq_pending(RingState *ring) {
  ring->sq_head_cache = __atomic_load_n(&ring->sq->Head, __ATOMIC_ACQUIRE);
  return ring->sq_tail - ring->sq_head_cache;
}

// Build a READ SQE with raw handle and buffer pointer.
// Only sets fields the kernel validates for unregistered I/O — _Padding,
// Key, and _Reserved are ignored by the kernel for non-registered ops.
LIBC_INLINE NT_IORING_SQE *push_read(RingState *ring, HANDLE file,
                                      void *buffer, ULONG length,
                                      ULONGLONG offset, ULONGLONG user_data,
                                      ULONG flags = 0) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  sqe->OpCode = IORING_OP_READ;
  sqe->Flags = flags;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags = 0;
  sqe->ReadWrite.FileRef = reinterpret_cast<ULONGLONG>(file);
  sqe->ReadWrite.BufferRef = reinterpret_cast<ULONGLONG>(buffer);
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;
  return sqe;
}

// Build a READ SQE with registered buffer only (raw file handle).
// Use when buffers are pre-registered but the file handle is not.
LIBC_INLINE NT_IORING_SQE *push_read_regbuf(RingState *ring, HANDLE file,
                                             ULONG buf_index,
                                             ULONG buf_offset, ULONG length,
                                             ULONGLONG offset,
                                             ULONGLONG user_data,
                                             ULONG flags = 0) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  sqe->OpCode = IORING_OP_READ;
  sqe->Flags = flags;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags = IORING_OP_FLAG_REGISTERED_BUFFER;
  sqe->ReadWrite.FileRef = reinterpret_cast<ULONGLONG>(file);
  sqe->ReadWrite.BufferRef =
      static_cast<ULONGLONG>(buf_index) |
      (static_cast<ULONGLONG>(buf_offset) << 32);
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;
  sqe->ReadWrite.Key = 0;
  return sqe;
}

// Build a READ SQE with registered file handle and buffer indices.
LIBC_INLINE NT_IORING_SQE *push_read_registered(RingState *ring,
                                                  ULONG file_index,
                                                  ULONG buf_index,
                                                  ULONG buf_offset,
                                                  ULONG length,
                                                  ULONGLONG offset,
                                                  ULONGLONG user_data,
                                                  ULONG flags = 0) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  sqe->OpCode = IORING_OP_READ;
  sqe->Flags = flags;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags =
      IORING_OP_FLAG_REGISTERED_FILE | IORING_OP_FLAG_REGISTERED_BUFFER;
  sqe->ReadWrite.FileRef = file_index;
  sqe->ReadWrite.BufferRef =
      static_cast<ULONGLONG>(buf_index) |
      (static_cast<ULONGLONG>(buf_offset) << 32);
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;
  sqe->ReadWrite.Key = 0;
  return sqe;
}

// Build a WRITE SQE with raw handle and buffer pointer.
// Only sets fields the kernel validates for unregistered I/O.
LIBC_INLINE NT_IORING_SQE *push_write(RingState *ring, HANDLE file,
                                       const void *buffer, ULONG length,
                                       ULONGLONG offset, ULONGLONG user_data,
                                       ULONG flags = 0) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  sqe->OpCode = IORING_OP_WRITE;
  sqe->Flags = flags;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags = 0;
  sqe->ReadWrite.FileRef = reinterpret_cast<ULONGLONG>(file);
  sqe->ReadWrite.BufferRef =
      reinterpret_cast<ULONGLONG>(const_cast<void *>(buffer));
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;
  return sqe;
}

// Build a WRITE SQE with registered buffer only (raw file handle).
LIBC_INLINE NT_IORING_SQE *push_write_regbuf(RingState *ring, HANDLE file,
                                              ULONG buf_index,
                                              ULONG buf_offset, ULONG length,
                                              ULONGLONG offset,
                                              ULONGLONG user_data,
                                              ULONG flags = 0) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  sqe->OpCode = IORING_OP_WRITE;
  sqe->Flags = flags;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags = IORING_OP_FLAG_REGISTERED_BUFFER;
  sqe->ReadWrite.FileRef = reinterpret_cast<ULONGLONG>(file);
  sqe->ReadWrite.BufferRef =
      static_cast<ULONGLONG>(buf_index) |
      (static_cast<ULONGLONG>(buf_offset) << 32);
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;
  sqe->ReadWrite.Key = 0;
  return sqe;
}

// Build a WRITE SQE with registered file handle and buffer indices.
LIBC_INLINE NT_IORING_SQE *push_write_registered(RingState *ring,
                                                   ULONG file_index,
                                                   ULONG buf_index,
                                                   ULONG buf_offset,
                                                   ULONG length,
                                                   ULONGLONG offset,
                                                   ULONGLONG user_data,
                                                   ULONG flags = 0) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  sqe->OpCode = IORING_OP_WRITE;
  sqe->Flags = flags;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags =
      IORING_OP_FLAG_REGISTERED_FILE | IORING_OP_FLAG_REGISTERED_BUFFER;
  sqe->ReadWrite.FileRef = file_index;
  sqe->ReadWrite.BufferRef =
      static_cast<ULONGLONG>(buf_index) |
      (static_cast<ULONGLONG>(buf_offset) << 32);
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;
  sqe->ReadWrite.Key = 0;
  return sqe;
}

// Build a CANCEL SQE. Cold path — zero the full union.
LIBC_INLINE NT_IORING_SQE *push_cancel(RingState *ring, HANDLE file,
                                        ULONGLONG cancel_id,
                                        ULONGLONG user_data) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  __builtin_memset(sqe, 0, sizeof(*sqe));
  sqe->OpCode = IORING_OP_CANCEL;
  sqe->UserData = user_data;
  sqe->Cancel.FileRef = reinterpret_cast<ULONGLONG>(file);
  sqe->Cancel.CancelId = cancel_id;
  return sqe;
}

// Build a FLUSH SQE. Cold path — zero the full union.
LIBC_INLINE NT_IORING_SQE *push_flush(RingState *ring, HANDLE file,
                                      ULONGLONG user_data,
                                      ULONG flush_mode =
                                          FLUSH_FLAGS_FILE_NORMAL,
                                      ULONG flags = 0) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  __builtin_memset(sqe, 0, sizeof(*sqe));
  sqe->OpCode = IORING_OP_FLUSH;
  sqe->Flags = flags;
  sqe->UserData = user_data;
  sqe->Flush.FlushMode = flush_mode;
  sqe->Flush.FileRef = reinterpret_cast<ULONGLONG>(file);
  return sqe;
}

// Build a REGISTER_FILES SQE. Cold path — zero the full union.
LIBC_INLINE NT_IORING_SQE *push_register_files(RingState *ring,
                                                 const HANDLE *handles,
                                                 ULONG count,
                                                 ULONGLONG user_data) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  __builtin_memset(sqe, 0, sizeof(*sqe));
  sqe->OpCode = IORING_OP_REGISTER_FILES;
  sqe->UserData = user_data;
  sqe->RegisterFiles.Handles =
      reinterpret_cast<ULONGLONG>(const_cast<HANDLE *>(handles));
  sqe->RegisterFiles.Count = count;
  return sqe;
}

// Build a REGISTER_BUFFERS SQE. Cold path — zero the full union.
LIBC_INLINE NT_IORING_SQE *push_register_buffers(RingState *ring,
                                                   const IORING_BUFFER_INFO *bufs,
                                                   ULONG count,
                                                   ULONGLONG user_data) {
  NT_IORING_SQE *sqe = acquire_sqe(ring);
  if (!sqe)
    return nullptr;
  __builtin_memset(sqe, 0, sizeof(*sqe));
  sqe->OpCode = IORING_OP_REGISTER_BUFFERS;
  sqe->UserData = user_data;
  sqe->RegisterBuffers.Buffers =
      reinterpret_cast<ULONGLONG>(const_cast<IORING_BUFFER_INFO *>(bufs));
  sqe->RegisterBuffers.Count = count;
  return sqe;
}

//===----------------------------------------------------------------------===//
// Submit — the only syscall in the hot path
//===----------------------------------------------------------------------===//

// Submit pending SQEs. wait_ops > 0 blocks until that many CQEs are posted.
// Flushes any deferred CQ Head update before submitting, so the kernel sees
// the reclaimed CQ slots and won't stall on a full CQ.
LIBC_INLINE NTSTATUS submit(RingState *ring, ULONG wait_ops = 0,
                            LARGE_INTEGER *timeout = nullptr) {
  // Flush deferred CQ Head — the kernel needs to see reclaimed slots.
  if (ring->cq_head != ring->cq_head_published) {
    __atomic_store_n(&ring->cq->Head, ring->cq_head, __ATOMIC_RELEASE);
    ring->cq_head_published = ring->cq_head;
  }
  // Release fence: SQE writes visible to the kernel before tail update.
  __atomic_store_n(&ring->sq->Tail, ring->sq_tail, __ATOMIC_RELEASE);
  return NtSubmitIoRing(ring->nt_handle, 0, wait_ops, timeout);
}

// Slim single-op submit + pop: combined submit → pop for the common case
// where exactly one SQE was just pushed and we expect a synchronous CQE.
//
// Skips:
//   - CQ Head flush (no-op after drain — cq_head == cq_head_published)
//   - cq_tail_cache check in pop (always stale after submit — kernel just
//     wrote a new CQE, but our cached tail is from before submit)
//
// On success: writes the CQE to *out, advances cq_head, returns true.
// On miss (async I/O): returns false — caller falls through to slow path.
LIBC_INLINE bool submit_and_pop_single(RingState *ring, NT_IORING_CQE *out) {
  // Flush deferred CQ Head — the kernel needs to see reclaimed slots
  // to post new CQEs. Without this, the CQ fills after CQ_SIZE ops.
  if (ring->cq_head != ring->cq_head_published) {
    __atomic_store_n(&ring->cq->Head, ring->cq_head, __ATOMIC_RELEASE);
    ring->cq_head_published = ring->cq_head;
  }
  // Publish SQ tail — kernel sees the new SQE.
  __atomic_store_n(&ring->sq->Tail, ring->sq_tail, __ATOMIC_RELEASE);
  NTSTATUS s = NtSubmitIoRing(ring->nt_handle, 0, 0, nullptr);
  if (!NT_SUCCESS(s))
    return false;

  // Direct CQ read — skip cq_tail_cache (guaranteed stale after submit).
  // Atomic acquire load of CQ Tail to see the kernel's new entry.
  ULONG cq_tail = __atomic_load_n(&ring->cq->Tail, __ATOMIC_ACQUIRE);
  if (ring->cq_head == cq_tail)
    return false; // Async — CQE not yet posted.

  *out = ring->cq->Entries[ring->cq_head & ring->cq_mask];
  ring->cq_head++;
  // Update cq_tail_cache so subsequent pop_cqe calls see the right state.
  ring->cq_tail_cache = cq_tail;
  return true;
}

//===----------------------------------------------------------------------===//
// CQE pop — pure userspace
//===----------------------------------------------------------------------===//

// Pop one CQE. Returns true if a CQE was available.
//
// Uses cached CQ.Tail — only re-reads from shared memory when the ring
// appears empty. CQ Head update is deferred (local cq_head only) — the
// release-store to shared memory happens in submit() before the next
// NtSubmitIoRing, saving one cache-line transfer per pop on the fast path.
LIBC_INLINE bool pop_cqe(RingState *ring, NT_IORING_CQE *out) {
  ULONG head = ring->cq_head;

  if (head == ring->cq_tail_cache) {
    // Ring appears empty — re-read tail from shared memory.
    ring->cq_tail_cache = __atomic_load_n(&ring->cq->Tail, __ATOMIC_ACQUIRE);
    if (head == ring->cq_tail_cache)
      return false;
  }

  *out = ring->cq->Entries[head & ring->cq_mask];
  ring->cq_head = head + 1;
  // Head store deferred to submit() — kernel doesn't need it until then.
  return true;
}

// Flush deferred CQ Head to shared memory. Call before waiting on the
// ring event when the kernel may need CQ slots to post async completions.
LIBC_INLINE void flush_cq_head(RingState *ring) {
  if (ring->cq_head != ring->cq_head_published) {
    __atomic_store_n(&ring->cq->Head, ring->cq_head, __ATOMIC_RELEASE);
    ring->cq_head_published = ring->cq_head;
  }
}

//===----------------------------------------------------------------------===//
// CQ readiness — non-consuming peek for poll/select
//===----------------------------------------------------------------------===//

// Check if any CQEs are available without consuming them.
// Refreshes cq_tail_cache — use for poll(POLLIN) semantics.
LIBC_INLINE bool cq_has_entries(RingState *ring) {
  if (ring->cq_head != ring->cq_tail_cache)
    return true;
  ring->cq_tail_cache = __atomic_load_n(&ring->cq->Tail, __ATOMIC_ACQUIRE);
  return ring->cq_head != ring->cq_tail_cache;
}

// Return number of unconsumed CQEs. Refreshes cq_tail_cache.
LIBC_INLINE ULONG cq_ready_count(RingState *ring) {
  ring->cq_tail_cache = __atomic_load_n(&ring->cq->Tail, __ATOMIC_ACQUIRE);
  return ring->cq_tail_cache - ring->cq_head;
}

// Peek at the next CQE without consuming it. Returns false if CQ is empty.
// The peeked entry remains in the ring — subsequent pop_cqe will return it.
LIBC_INLINE bool peek_cqe(RingState *ring, NT_IORING_CQE *out) {
  if (ring->cq_head == ring->cq_tail_cache) {
    ring->cq_tail_cache = __atomic_load_n(&ring->cq->Tail, __ATOMIC_ACQUIRE);
    if (ring->cq_head == ring->cq_tail_cache)
      return false;
  }
  *out = ring->cq->Entries[ring->cq_head & ring->cq_mask];
  return true;
}

//===----------------------------------------------------------------------===//
// Capability query
//===----------------------------------------------------------------------===//

LIBC_INLINE NTSTATUS query_capabilities(NT_IORING_CAPABILITIES *caps) {
  return NtQueryIoRingCapabilities(sizeof(*caps), caps);
}

} // namespace ioring
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_OPS_H
