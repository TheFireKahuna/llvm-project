//===-- IO Ring shared helpers -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared IO Ring utilities for the FILE* layer and the fd layer:
//   - cqe_status_to_errno: map CQE NTSTATUS to POSIX errno
//   - drain_stale_cqes: discard CQEs from previous generations
//   - ioring_submit: submit SQEs + fast-path CQE pop with generation check
//   - ioring_wait_cqe: alertable wait + generation-aware EINTR cancellation
//   - ioring_submit_and_wait: combines submit + wait (convenience wrapper)
//   - open_overlapped: open a file with overlapped I/O
//   - open_sync_alertable: open with FILE_SYNCHRONOUS_IO_ALERT (emulation)
//   - is_ioring_emulated: check for kernel IO Ring emulation
//
// All submit/wait helpers accept a generation counter (from ThreadRing) to
// validate CQE provenance. Stale CQEs from cancelled previous operations
// are silently discarded — no timeout-based drain heuristics needed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_RING_HELPERS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_RING_HELPERS_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/File/file_io_result.h"
#include "src/__support/OSUtil/windows/io/alertable_io.h"
#include "src/__support/OSUtil/windows/io/op_tag.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/signal/syscall_frame.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/thread_lifecycle.h"

//===----------------------------------------------------------------------===//
// CQE result code translation
//===----------------------------------------------------------------------===//

// Map CQE ResultCode to POSIX errno.
//
// IO Ring CQE ResultCode is an HRESULT, not a raw NTSTATUS. Three formats:
//   - HRESULT_FROM_NT(status): bit 28 (0x10000000) set, status in low 28 bits
//   - HRESULT_FROM_WIN32(err): facility=7, error code in low 16 bits
//   - Raw NTSTATUS: some drivers return these directly (bit 28 clear)
//
// We normalize HRESULT_FROM_NT values back to raw NTSTATUS and route through
// the centralized ntstatus_to_errno(), with an override table for the few
// Win32-origin HRESULTs that can't be extracted.
// STATUS_END_OF_FILE as both raw NTSTATUS and HRESULT_FROM_NT encoding.
// IoRing CQE ResultCode may use either form depending on the storage driver.
inline constexpr LONG NT_END_OF_FILE = static_cast<LONG>(0xC0000011);
inline constexpr LONG HR_END_OF_FILE = static_cast<LONG>(0xD0000011);

LIBC_INLINE int cqe_status_to_errno(LONG hr) {
  // HRESULT_FROM_NT: bit 28 set + severity bits [31:30] = 11.
  // Extract raw NTSTATUS by clearing the N bit (bit 28).
  constexpr LONG HRESULT_NT_BIT = 0x10000000;
  if ((hr & 0xF0000000L) == 0xD0000000L) {
    NTSTATUS raw = static_cast<NTSTATUS>(hr & ~HRESULT_NT_BIT);
    return LIBC_NAMESPACE::windows_util::ntstatus_to_errno(raw);
  }

  // Raw NTSTATUS (severity = 0xC0000000, N bit clear).
  if ((hr & 0xC0000000L) == 0xC0000000L)
    return LIBC_NAMESPACE::windows_util::ntstatus_to_errno(
        static_cast<NTSTATUS>(hr));

  // HRESULT_FROM_WIN32: facility=7 (0x8007xxxx).
  if ((hr & 0xFFFF0000L) == 0x80070000L) {
    int win32_err = hr & 0xFFFF;
    switch (win32_err) {
    case 2:
      return ENOENT; // ERROR_FILE_NOT_FOUND
    case 3:
      return ENOENT; // ERROR_PATH_NOT_FOUND
    case 5:
      return EACCES; // ERROR_ACCESS_DENIED
    case 6:
      return EBADF; // ERROR_INVALID_HANDLE
    case 14:
      return ENOMEM; // ERROR_OUTOFMEMORY
    case 32:
      return EBUSY; // ERROR_SHARING_VIOLATION
    case 33:
      return EBUSY; // ERROR_LOCK_VIOLATION
    case 87:
      return EINVAL; // ERROR_INVALID_PARAMETER
    case 109:
      return EPIPE; // ERROR_BROKEN_PIPE
    case 112:
      return ENOSPC; // ERROR_DISK_FULL
    case 122:
      return ENOMEM; // ERROR_INSUFFICIENT_BUFFER
    case 183:
      return EEXIST; // ERROR_ALREADY_EXISTS
    case 232:
      return EPIPE; // ERROR_NO_DATA (pipe closing)
    case 1392:
      return EIO; // ERROR_FILE_CORRUPT
    default:
      return EIO;
    }
  }

  return EIO;
}

//===----------------------------------------------------------------------===//
// CQE interpretation — generation-aware
//===----------------------------------------------------------------------===//

// Interpret a CQE as a FileIOResult, mapping EOF and errors.
LIBC_INLINE bool is_eof_result(LONG rc) {
  return rc == NT_END_OF_FILE || rc == HR_END_OF_FILE;
}

LIBC_INLINE LIBC_NAMESPACE::FileIOResult cqe_to_result(const NT_IORING_CQE &cqe) {
  // Success (ResultCode == 0) is the common case — check first.
  if (LIBC_LIKELY(cqe.ResultCode == 0))
    return static_cast<size_t>(cqe.Information);
  if (is_eof_result(cqe.ResultCode))
    return size_t{0};
  return {0, cqe_status_to_errno(cqe.ResultCode)};
}

//===----------------------------------------------------------------------===//
// Stale CQE drain
//===----------------------------------------------------------------------===//

// Pop and discard all CQEs currently in the ring. Called before batch
// submissions to guarantee CQ headroom. For single-op paths, generation
// checking handles staleness lazily (no pre-drain needed since CQ capacity
// 128 >> 1 SQE).
LIBC_INLINE void drain_stale_cqes(LIBC_NAMESPACE::ioring::RingState *ring) {
  NT_IORING_CQE discard;
  while (LIBC_NAMESPACE::ioring::pop_cqe(ring, &discard))
    ; // Silently discard all stale entries.
}

// Pop CQEs until one matching the expected generation is found.
// Stale CQEs (wrong generation) are silently skipped.
// Returns true if a matching CQE was found and written to *out.
LIBC_INLINE bool pop_cqe_gen(LIBC_NAMESPACE::ioring::RingState *ring,
                             uint32_t gen, NT_IORING_CQE *out) {
  NT_IORING_CQE cqe;
  while (LIBC_NAMESPACE::ioring::pop_cqe(ring, &cqe)) {
    auto tag = LIBC_NAMESPACE::ioring::OpTag::from_user_data(cqe.UserData);
    if (tag.is_generation(gen)) {
      *out = cqe;
      return true;
    }
    // Stale CQE — discard silently.
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Combined push+submit+pop — single-op fast path
//===----------------------------------------------------------------------===//
//
// Merges SQE fill → CQ head flush → SQ tail publish → NtSubmitIoRing →
// CQ read → result interpretation into one tight function. The compiler
// sees a single straight-line block with one syscall in the middle,
// enabling better register allocation and instruction scheduling vs the
// split push_read → ioring_submit → cqe_to_result chain.
//
// Returns FileIOResult on fast path (cached I/O — CQE ready on return).
// Returns {0, EAGAIN} on slow path (async — caller falls to wait).

// Internal: fill SQE + submit + pop + futex spin/park. One function,
// no handoff gap where the completion event can be lost.
//
// Returns FileIOResult on success/error.
// Returns {0, EAGAIN} only if futex spin/park exhausted — caller must
// fall to alertable ioring_wait_cqe (rarest case: very slow storage).
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
read_single_op(LIBC_NAMESPACE::ioring::RingState *ring,
               HANDLE file, void *buffer, ULONG length, ULONGLONG offset,
               uint32_t gen) {
  ULONGLONG user_data =
      LIBC_NAMESPACE::ioring::OpTag{gen, 0}.as_user_data();
  // --- SQE fill (inlined, no acquire_sqe full-check) ---
  NT_IORING_SQE *sqe = &ring->sq->Entries[ring->sq_tail & ring->sq_mask];
  ring->sq_tail++;
  sqe->OpCode = IORING_OP_READ;
  sqe->Flags = 0;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags = 0;
  sqe->ReadWrite.FileRef = reinterpret_cast<ULONGLONG>(file);
  sqe->ReadWrite.BufferRef = reinterpret_cast<ULONGLONG>(buffer);
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;

  // --- Submit ---
  if (ring->cq_head != ring->cq_head_published) {
    __atomic_store_n(&ring->cq->Head, ring->cq_head, __ATOMIC_RELEASE);
    ring->cq_head_published = ring->cq_head;
  }
  __atomic_store_n(&ring->sq->Tail, ring->sq_tail, __ATOMIC_RELEASE);
  NTSTATUS s = NtSubmitIoRing(ring->nt_handle, 0, 0, nullptr);
  if (!NT_SUCCESS(s))
    return {0, EIO};

  // --- Fast pop (cached I/O: CQE ready on return) ---
  ULONG cq_tail = __atomic_load_n(&ring->cq->Tail, __ATOMIC_ACQUIRE);
  if (ring->cq_head != cq_tail) {
    NT_IORING_CQE cqe = ring->cq->Entries[ring->cq_head & ring->cq_mask];
    ring->cq_head++;
    ring->cq_tail_cache = cq_tail;
    if (LIBC_LIKELY(cqe.ResultCode == 0))
      return static_cast<size_t>(cqe.Information);
    if (is_eof_result(cqe.ResultCode))
      return size_t{0};
    return {0, cqe_status_to_errno(cqe.ResultCode)};
  }

  // --- Medium path: parking lot wait via reactor bridge ---
  LIBC_NAMESPACE::ioring::flush_cq_head(ring);
  __atomic_store_n(&ring->needs_wake,
                   LIBC_NAMESPACE::ioring::WAKE_FUTEX, __ATOMIC_RELEASE);
  LIBC_NAMESPACE::futex_addr::wait_nt(&ring->cq->Tail, cq_tail, nullptr);
  __atomic_store_n(&ring->needs_wake,
                   LIBC_NAMESPACE::ioring::WAKE_NONE, __ATOMIC_RELAXED);

  NT_IORING_CQE cqe;
  if (pop_cqe_gen(ring, gen, &cqe)) {
    if (LIBC_LIKELY(cqe.ResultCode == 0))
      return static_cast<size_t>(cqe.Information);
    if (is_eof_result(cqe.ResultCode))
      return size_t{0};
    return {0, cqe_status_to_errno(cqe.ResultCode)};
  }

  return {0, EAGAIN};
}

LIBC_INLINE LIBC_NAMESPACE::FileIOResult
write_single_op(LIBC_NAMESPACE::ioring::RingState *ring,
                HANDLE file,
                const void *buffer, ULONG length, ULONGLONG offset,
                uint32_t gen) {
  ULONGLONG user_data =
      LIBC_NAMESPACE::ioring::OpTag{gen, 0}.as_user_data();
  NT_IORING_SQE *sqe = &ring->sq->Entries[ring->sq_tail & ring->sq_mask];
  ring->sq_tail++;
  sqe->OpCode = IORING_OP_WRITE;
  sqe->Flags = 0;
  sqe->UserData = user_data;
  sqe->ReadWrite.CommonOpFlags = 0;
  sqe->ReadWrite.FileRef = reinterpret_cast<ULONGLONG>(file);
  sqe->ReadWrite.BufferRef =
      reinterpret_cast<ULONGLONG>(const_cast<void *>(buffer));
  sqe->ReadWrite.Offset = offset;
  sqe->ReadWrite.Length = length;

  if (ring->cq_head != ring->cq_head_published) {
    __atomic_store_n(&ring->cq->Head, ring->cq_head, __ATOMIC_RELEASE);
    ring->cq_head_published = ring->cq_head;
  }
  __atomic_store_n(&ring->sq->Tail, ring->sq_tail, __ATOMIC_RELEASE);
  NTSTATUS s = NtSubmitIoRing(ring->nt_handle, 0, 0, nullptr);
  if (!NT_SUCCESS(s))
    return {0, EIO};

  ULONG cq_tail = __atomic_load_n(&ring->cq->Tail, __ATOMIC_ACQUIRE);
  if (ring->cq_head != cq_tail) {
    NT_IORING_CQE cqe = ring->cq->Entries[ring->cq_head & ring->cq_mask];
    ring->cq_head++;
    ring->cq_tail_cache = cq_tail;
    if (LIBC_LIKELY(cqe.ResultCode == 0))
      return static_cast<size_t>(cqe.Information);
    if (is_eof_result(cqe.ResultCode))
      return size_t{0};
    return {0, cqe_status_to_errno(cqe.ResultCode)};
  }

  LIBC_NAMESPACE::ioring::flush_cq_head(ring);
  __atomic_store_n(&ring->needs_wake,
                   LIBC_NAMESPACE::ioring::WAKE_FUTEX, __ATOMIC_RELEASE);
  LIBC_NAMESPACE::futex_addr::wait_nt(&ring->cq->Tail, cq_tail, nullptr);
  __atomic_store_n(&ring->needs_wake,
                   LIBC_NAMESPACE::ioring::WAKE_NONE, __ATOMIC_RELAXED);

  NT_IORING_CQE cqe;
  if (pop_cqe_gen(ring, gen, &cqe)) {
    if (LIBC_LIKELY(cqe.ResultCode == 0))
      return static_cast<size_t>(cqe.Information);
    if (is_eof_result(cqe.ResultCode))
      return size_t{0};
    return {0, cqe_status_to_errno(cqe.ResultCode)};
  }

  return {0, EAGAIN};
}

//===----------------------------------------------------------------------===//
// Single-op submit + wait — generation-aware (legacy path)
//===----------------------------------------------------------------------===//

// Hybrid submit: submit_nowait + CQ poll + futex_addr spin/park.
//
// Fast path (cached I/O): submit_nowait processes SQEs synchronously —
// CQE is already in the ring on return. One pop, no kernel wait. +30%
// faster than submit_wait(1) which adds ~2,000 cycles of unnecessary
// kernel wait overhead even when the CQE is ready. (Research Section 10)
//
// Medium path (NVMe/fast storage): CQE arrives in ~5-20μs. futex_addr
// spin phase (32 cpu_relax iterations, ~1μs) catches it without a
// kernel wait syscall.
//
// Slow path (disk seek/network): spin exhausted, futex_addr parks via
// NtWaitForAlertByThreadId. Woken by completion event → reactor bridge
// calling futex_addr::wake(&cq->Tail, 1). The bridge is registered in
// thread_ring_create(): reactor::watch(event, ioring_cq_waker, tr).
//
// Returns the result if a matching CQE was found.
// Returns {0, EAGAIN} if CQE not available after spin+park (caller
// must fall back to alertable wait).
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
ioring_submit(LIBC_NAMESPACE::ioring::RingState *ring, uint32_t gen) {
  // Slim fast path: combined submit + pop in one call. Skips CQ head
  // flush (no-op after drain) and cq_tail_cache check (always stale).
  // For cached I/O the CQE is posted synchronously by the kernel.
  NT_IORING_CQE cqe;
  if (LIBC_NAMESPACE::ioring::submit_and_pop_single(ring, &cqe))
    return cqe_to_result(cqe);

  // CQE not yet available (uncached I/O, network). The CQ tail was
  // already loaded by submit_and_pop_single — use the cached value.
  ULONG old_tail = ring->cq_tail_cache;

  // Flush deferred CQ Head — the kernel needs to see reclaimed CQ slots.
  LIBC_NAMESPACE::ioring::flush_cq_head(ring);

  __atomic_store_n(&ring->needs_wake,
                   LIBC_NAMESPACE::ioring::WAKE_FUTEX, __ATOMIC_RELEASE);
  LIBC_NAMESPACE::futex_addr::wait_nt(&ring->cq->Tail, old_tail, nullptr);
  __atomic_store_n(&ring->needs_wake,
                   LIBC_NAMESPACE::ioring::WAKE_NONE, __ATOMIC_RELAXED);

  if (pop_cqe_gen(ring, gen, &cqe))
    return cqe_to_result(cqe);

  // CQE not available after spin+park — fall back to alertable wait.
  return {0, EAGAIN};
}

//===----------------------------------------------------------------------===//
// Registered buffer I/O — bounce-buffer path
//===----------------------------------------------------------------------===//

// Registered pread: acquire ring buffer region → push_read_regbuf →
// submit_nowait → pop CQE → memcpy to user buffer → release region.
//
// Uses IORING_OP_FLAG_REGISTERED_BUFFER with a raw HANDLE (no file
// registration — Section 6 shows ~0% cached benefit). The pre-pinned MDL
// eliminates per-op page table walks: 67% speedup for unbuffered 64K I/O.
//
// submit_nowait + CQ poll: for cached/standby I/O the CQE is posted
// synchronously — no kernel wait overhead. Falls back to futex_addr
// spin/park for uncached I/O (same path as ioring_submit).
//
// Returns the I/O result. Falls back to EAGAIN if the registered buffer
// ring has no space (caller should use unregistered path).
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
ioring_pread_registered(LIBC_NAMESPACE::ioring::ThreadRing *tr, HANDLE file,
                        void *buf, ULONG len, ULONGLONG offset,
                        uint32_t gen) {
  using namespace LIBC_NAMESPACE;

  int32_t buf_off = tr->reg_buf_acquire(len);
  if (buf_off < 0)
    return {0, EAGAIN}; // No space — caller falls back to unregistered.

  ioring::RingState *ring = &tr->ring;
  ioring::OpTag tag{gen, 0};

  auto *sqe = ioring::push_read_regbuf(ring, file, /*buf_index=*/0,
                                        static_cast<ULONG>(buf_off), len,
                                        offset, tag.as_user_data());
  if (!sqe) {
    tr->reg_buf_release(len);
    return {0, EIO};
  }

  // Slim single-op submit + pop: combined path skips CQ head flush
  // and stale cq_tail_cache check. For O_DIRECT the CQE is typically
  // async, but NVMe can complete synchronously for small aligned reads.
  NT_IORING_CQE cqe;
  if (!ioring::submit_and_pop_single(ring, &cqe)) {
    ULONG old_tail = ring->cq_tail_cache;
    ioring::flush_cq_head(ring);
    __atomic_store_n(&ring->needs_wake,
                     LIBC_NAMESPACE::ioring::WAKE_FUTEX, __ATOMIC_RELEASE);
    futex_addr::wait_nt(&ring->cq->Tail, old_tail, nullptr);
    __atomic_store_n(&ring->needs_wake,
                     LIBC_NAMESPACE::ioring::WAKE_NONE, __ATOMIC_RELAXED);
    if (!pop_cqe_gen(ring, gen, &cqe)) {
      tr->reg_buf_release(len);
      return {0, EIO};
    }
  }

  FileIOResult result = cqe_to_result(cqe);

  // Copy data from registered bounce buffer to the caller's buffer.
  if (result.value > 0)
    __builtin_memcpy(buf, tr->reg_buf_ptr(static_cast<uint32_t>(buf_off)),
                     result.value);

  tr->reg_buf_release(len);
  return result;
}

// Registered pwrite: memcpy from user buffer → acquire ring buffer region →
// push_write_regbuf → submit_nowait → pop CQE → release region.
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
ioring_pwrite_registered(LIBC_NAMESPACE::ioring::ThreadRing *tr, HANDLE file,
                         const void *buf, ULONG len, ULONGLONG offset,
                         uint32_t gen) {
  using namespace LIBC_NAMESPACE;

  int32_t buf_off = tr->reg_buf_acquire(len);
  if (buf_off < 0)
    return {0, EAGAIN};

  // Copy data into registered bounce buffer before submission.
  __builtin_memcpy(tr->reg_buf_ptr(static_cast<uint32_t>(buf_off)), buf, len);

  ioring::RingState *ring = &tr->ring;
  ioring::OpTag tag{gen, 0};

  auto *sqe = ioring::push_write_regbuf(ring, file, /*buf_index=*/0,
                                         static_cast<ULONG>(buf_off), len,
                                         offset, tag.as_user_data());
  if (!sqe) {
    tr->reg_buf_release(len);
    return {0, EIO};
  }

  // Slim single-op submit + pop.
  NT_IORING_CQE cqe;
  if (!ioring::submit_and_pop_single(ring, &cqe)) {
    ULONG old_tail = ring->cq_tail_cache;
    ioring::flush_cq_head(ring);
    __atomic_store_n(&ring->needs_wake,
                     LIBC_NAMESPACE::ioring::WAKE_FUTEX, __ATOMIC_RELEASE);
    futex_addr::wait_nt(&ring->cq->Tail, old_tail, nullptr);
    __atomic_store_n(&ring->needs_wake,
                     LIBC_NAMESPACE::ioring::WAKE_NONE, __ATOMIC_RELAXED);
    if (!pop_cqe_gen(ring, gen, &cqe)) {
      tr->reg_buf_release(len);
      return {0, EIO};
    }
  }

  FileIOResult result = cqe_to_result(cqe);
  tr->reg_buf_release(len);
  return result;
}

//===----------------------------------------------------------------------===//
// Alertable wait
//===----------------------------------------------------------------------===//

// Alertable wait for a CQE on |ring|. Handles SA_RESTART and EINTR
// cancellation with generation-aware CQE filtering.
//
// |wait_event| is a separate SynchronizationEvent signaled by the reactor
// waker callback (ioring_cq_waker) when needs_wake == WAKE_EVENT. This is
// NOT the IoRing completion event — using the IoRing event here would race
// with the reactor WCP (both are consumers of the same auto-reset event,
// so the WCP can steal the signal and the NtWaitForSingleObject never wakes).
//
// |event| is the IoRing completion event, used only for the EINTR CQE drain
// (short timeout, not hang-critical).
//
// Installs a SyscallFrameGuard (RAII) so pthread_cancel and fork can
// cancel the in-flight SQE if needed. The frame is stack-allocated and
// linked via prev — safe for nested syscalls in signal handlers.
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
ioring_wait_cqe(LIBC_NAMESPACE::ioring::RingState *ring, HANDLE event,
                HANDLE wait_event, HANDLE file_handle, uint32_t gen) {
  using namespace LIBC_NAMESPACE;
  using namespace LIBC_NAMESPACE::signal_state;

  // Install SyscallFrame via RAII guard — automatically links on
  // construction and unlinks on destruction (any exit path).
  ioring::OpTag tag{gen, 0};
  SyscallFrameGuard frame_guard(file_handle, ring, event,
                                tag.as_user_data(), /*batch_count=*/0);

  for (;;) {
    // Try popping a matching CQE before waiting (covers the case where
    // the event was already signaled or CQEs arrived between iterations).
    NT_IORING_CQE cqe;
    if (pop_cqe_gen(ring, gen, &cqe))
      return cqe_to_result(cqe);

    // Flush deferred CQ Head before blocking — the kernel needs to see
    // reclaimed CQ slots to post async completions.
    ioring::flush_cq_head(ring);

    // Tell the reactor waker to signal wait_event (not the futex).
    __atomic_store_n(&ring->needs_wake,
                     LIBC_NAMESPACE::ioring::WAKE_EVENT, __ATOMIC_RELEASE);

    NTSTATUS status = ::NtWaitForSingleObject(wait_event, /*Alertable=*/1,
                                              /*Timeout=*/nullptr);

    __atomic_store_n(&ring->needs_wake,
                     LIBC_NAMESPACE::ioring::WAKE_NONE, __ATOMIC_RELAXED);

    if (status == STATUS_SUCCESS) {
      if (pop_cqe_gen(ring, gen, &cqe))
        return cqe_to_result(cqe);
      continue; // Spurious wake or waker fired before CQE visible. Retry.
    }

    if (status == STATUS_USER_APC) {
      if (should_restart_syscall())
        continue;

      // Cancel the in-flight SQE using a generation-tagged cancel tag.
      auto cancel_tag = ioring::OpTag::make_cancel(gen);
      ioring::push_cancel(ring, file_handle, tag.as_user_data(),
                          cancel_tag.as_user_data());
      ioring::submit(ring);

      // Drain CQEs — both the original op and the cancel op may produce
      // completions. Generation filtering means we only need to drain
      // CQEs from this generation; stale ones from prior generations are
      // skipped automatically by pop_cqe_gen.
      //
      // Uses the IoRing completion event (not wait_event) — short timeout,
      // not hang-critical. The WCP may consume some signals but we always
      // check CQEs and break on timeout.
      for (int attempts = 0; attempts < 4; ++attempts) {
        if (pop_cqe_gen(ring, gen, &cqe))
          continue; // Got one — keep draining.
        // Wait briefly for stragglers.
        LARGE_INTEGER timeout;
        timeout.QuadPart = -50LL * 10000LL; // 50ms
        NTSTATUS ws =
            ::NtWaitForSingleObject(event, /*Alertable=*/0, &timeout);
        if (ws == STATUS_TIMEOUT)
          break; // Ring is clean enough — any remaining stale CQEs will
                 // be rejected by generation check on next operation.
      }

      return {0, EINTR};
    }

    // Unexpected wait status.
    return {0, EIO};
  }
}

// Submit pending SQEs and wait for one completion.
// Fast path: submit(ring, 1) — single syscall, CQE available on return.
// Falls back to alertable event wait if fast path returns EAGAIN.
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
ioring_submit_and_wait(LIBC_NAMESPACE::ioring::RingState *ring, HANDLE event,
                       HANDLE wait_event, HANDLE file_handle, uint32_t gen) {
  auto result = ioring_submit(ring, gen);
  if (result.error != EAGAIN)
    return result;
  if (!wait_event)
    return {0, EIO};
  return ioring_wait_cqe(ring, event, wait_event, file_handle, gen);
}

// Combined single-op + alertable wait. Uses the fully-inlined fast path
// (read/write_single_op), falls back to alertable wait if async.
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
read_single_op_wait(LIBC_NAMESPACE::ioring::RingState *ring, HANDLE event,
                    HANDLE wait_event, HANDLE file, void *buffer, ULONG length,
                    ULONGLONG offset, uint32_t gen) {
  auto result = read_single_op(ring, file, buffer, length, offset, gen);
  if (result.error != EAGAIN)
    return result;
  if (!wait_event)
    return {0, EIO};
  return ioring_wait_cqe(ring, event, wait_event, file, gen);
}

LIBC_INLINE LIBC_NAMESPACE::FileIOResult
write_single_op_wait(LIBC_NAMESPACE::ioring::RingState *ring, HANDLE event,
                     HANDLE wait_event, HANDLE file, const void *buffer,
                     ULONG length, ULONGLONG offset, uint32_t gen) {
  auto result = write_single_op(ring, file, buffer, length, offset, gen);
  if (result.error != EAGAIN)
    return result;
  if (!wait_event)
    return {0, EIO};
  return ioring_wait_cqe(ring, event, wait_event, file, gen);
}

// Pipe I/O with O_NONBLOCK awareness for overlapped pipe handles.
//
// Overlapped handles complete asynchronously — submit(ring, 0) fires the
// SQE without blocking for a CQE. Three outcomes:
//
//   1. Synchronous completion (pipe had data/space): CQE immediately
//      available after submit. Return the result.
//
//   2. Pending + nonblock: cancel the in-flight SQE and return EAGAIN.
//      If the I/O completed in the race window between our pop and the
//      cancel, the data is already in the caller's buffer — return that
//      result instead of discarding it.
//
//   3. Pending + blocking: fall through to ioring_wait_cqe() for
//      alertable waiting (signal delivery, SA_RESTART, cancellation).
LIBC_INLINE LIBC_NAMESPACE::FileIOResult
ioring_pipe_io(LIBC_NAMESPACE::ioring::RingState *ring, HANDLE event,
               HANDLE wait_event, HANDLE file_handle, uint32_t gen,
               bool nonblock) {
  using namespace LIBC_NAMESPACE;

  // Fire-and-forget submit — overlapped handles don't block in-kernel.
  NTSTATUS s = ioring::submit(ring, 0);
  if (!NT_SUCCESS(s))
    return {0, EIO};

  // Fast path: CQE already available (synchronous completion).
  NT_IORING_CQE cqe;
  if (pop_cqe_gen(ring, gen, &cqe))
    return cqe_to_result(cqe);

  // I/O is pending in the kernel.
  if (nonblock) {
    // Cancel the in-flight SQE.
    ioring::OpTag tag{gen, 0};
    auto cancel_tag = ioring::OpTag::make_cancel(gen);
    ioring::push_cancel(ring, file_handle, tag.as_user_data(),
                        cancel_tag.as_user_data());
    ioring::submit(ring);

    // Drain CQEs from both the original and cancel operations.
    // If the original completed between our pop and the cancel (race),
    // capture its result — the data is already in the caller's buffer.
    FileIOResult data_result = {0, EAGAIN};
    for (int i = 0; i < 4; ++i) {
      if (!pop_cqe_gen(ring, gen, &cqe)) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -50LL * 10000LL; // 50ms
        NTSTATUS ws =
            ::NtWaitForSingleObject(event, /*Alertable=*/0, &timeout);
        if (ws == STATUS_TIMEOUT)
          break;
        if (!pop_cqe_gen(ring, gen, &cqe))
          break;
      }
      // Non-cancel CQE with success → the read/write completed.
      auto t = ioring::OpTag::from_user_data(cqe.UserData);
      if (!t.is_cancel() && cqe.ResultCode >= 0)
        data_result = cqe_to_result(cqe);
    }
    return data_result;
  }

  // Blocking: alertable wait for completion.
  if (!wait_event)
    return {0, EIO};
  return ioring_wait_cqe(ring, event, wait_event, file_handle, gen);
}

//===----------------------------------------------------------------------===//
// IO Ring emulation detection
//===----------------------------------------------------------------------===//

// Check whether IO Ring is kernel-emulated (Hyper-V guests, some storage
// drivers). Cached after first call. When emulated, per-operation overhead
// is worse than sync I/O — callers should fall back to sync handles.
LIBC_INLINE bool is_ioring_emulated() {
  // 0 = unknown, 1 = native, 2 = emulated
  static LIBC_NAMESPACE::cpp::Atomic<int> cached{0};
  int val = cached.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE);
  if (val != 0)
    return val == 2;

  NT_IORING_CAPABILITIES caps = {};
  NTSTATUS s = LIBC_NAMESPACE::ioring::query_capabilities(&caps);
  bool emulated =
      !NT_SUCCESS(s) || (caps.Features & IORING_FEATURE_UM_EMULATION) != 0;
  cached.store(emulated ? 2 : 1, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  return emulated;
}

//===----------------------------------------------------------------------===//
// File open helpers
//===----------------------------------------------------------------------===//

// Open a file via NtCreateFile (overlapped, no FILE_SYNCHRONOUS_IO_ALERT).
// IO Ring + event creation is handled by fd_table.alloc(), not here.
LIBC_INLINE NTSTATUS
open_overlapped(const OBJECT_ATTRIBUTES *oa, ACCESS_MASK access,
                ULONG disposition, ULONG share, HANDLE *out_handle,
                ULONG options = FILE_NON_DIRECTORY_FILE,
                ULONG file_attributes = FILE_ATTRIBUTE_NORMAL,
                ULONG_PTR *out_info = nullptr) {
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = NtCreateFile(out_handle, access, oa, &iosb, nullptr,
                            file_attributes, share, disposition, options,
                            nullptr, 0);
  if (s == STATUS_PENDING) {
    s = ::NtWaitForSingleObject(*out_handle, /*Alertable=*/0, nullptr);
    if (NT_SUCCESS(s))
      s = iosb.Status;
  }
  if (out_info)
    *out_info = iosb.Information;
  return s;
}

// Open a file with FILE_SYNCHRONOUS_IO_ALERT — synchronous, alertable,
// kernel-tracked position. Used as fallback when IO Ring is emulated.
LIBC_INLINE NTSTATUS
open_sync_alertable(const OBJECT_ATTRIBUTES *oa, ACCESS_MASK access,
                    ULONG disposition, ULONG share, HANDLE *out_handle,
                    ULONG options = FILE_NON_DIRECTORY_FILE,
                    ULONG file_attributes = FILE_ATTRIBUTE_NORMAL) {
  IO_STATUS_BLOCK iosb = {};
  return NtCreateFile(out_handle, access, oa, &iosb, nullptr, file_attributes,
                      share, disposition,
                      options | FILE_SYNCHRONOUS_IO_ALERT, nullptr, 0);
}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_RING_HELPERS_H
