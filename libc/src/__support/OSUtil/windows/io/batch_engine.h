//===-- Pipeline IoRing batch engine ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// BatchEngine accumulates N I/O operations and submits them through a
// pipeline: push up to QD SQEs, drain completed CQEs as they arrive, push
// replacements to keep the device busy, repeat until all ops finish.
//
// This is the E3 pipeline pattern from IORING_VS_NTREADFILE_RESEARCH.md:
// 99% faster than batch-drain (push all → wait all → drain all) because
// it never waits for count>1 — the completion event fires once per CQ
// empty→non-empty transition, so waiting for 1 CQE at a time and
// draining greedily is the only correct event-based strategy.
//
// For small batches (count <= PIPELINE_QD), all ops are pushed at once
// and submit_wait lets the kernel coalesce completions internally — same
// efficiency as the old batch-drain, one syscall, zero extra latency for
// cached I/O.
//
// Design constraints:
//
//   No fixed-size arrays — result tracking is bounded by BATCH_MAX_OPS
//   (decoupled from ring SQ capacity). The per-slot byte count array and
//   completion bitmap are sized to BATCH_MAX_OPS at compile time. The
//   pipeline pushes at most SQ_SIZE SQEs at once, refilling as CQEs drain.
//
//   Stack-allocated — no heap, no slab. BatchEngine is a local variable
//   inside readv/writev/preadv/pwritev/sendfile. Its lifetime is the
//   duration of one POSIX call.
//
//   CQEs arrive in arbitrary completion order (different files, different
//   storage paths, different latencies). The slot index encoded in UserData
//   maps each CQE back to its iovec element. After drain, results are
//   walked in submission order to find the longest successful prefix (POSIX
//   partial-transfer semantics).
//
//   EINTR: all in-flight SQEs are cancelled, their cancel CQEs drained,
//   and partial bytes returned. Pending (not yet pushed) ops are simply
//   not submitted. Generation tagging means leftover CQEs from this batch
//   are silently rejected by subsequent operations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_BATCH_ENGINE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_BATCH_ENGINE_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/File/file_io_result.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/op_tag.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/signal/syscall_frame.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"

namespace LIBC_NAMESPACE_DECL {
namespace ioring {

//===----------------------------------------------------------------------===//
// BatchResult — returned by BatchEngine::submit_and_drain()
//===----------------------------------------------------------------------===//

struct BatchResult {
  size_t total_bytes;   // Sum of bytes across the successful prefix
  int error;            // 0 on full success, errno on first failure or EINTR
  uint32_t completed;   // Number of ops in the successful prefix
};

//===----------------------------------------------------------------------===//
// BatchEngine
//===----------------------------------------------------------------------===//

class BatchEngine {
  // Result tracking: one entry per registered op. Sized to the ring's SQ
  // capacity — you cannot register more ops than the ring holds.
  struct SlotResult {
    size_t bytes;       // CQE.Information (bytes transferred)
    size_t requested;   // Bytes requested in this SQE (for short-read detect)
    int error;          // 0 on success, errno on failure
    bool completed;     // CQE received for this slot
  };

  // Deferred operation descriptor. Stored at push time, pushed to the
  // ring's SQ during pipeline fill/refill phases. This decoupling lets
  // the pipeline control how many SQEs are in flight at once.
  struct PendingOp {
    void *buf;          // Read target or write source
    ULONG len;
    ULONGLONG offset;
    enum : uint8_t { READ, WRITE, FLUSH } type;
    ULONG flush_mode;   // Only meaningful for FLUSH
  };

  // BATCH_MAX_OPS is the maximum ops per batch (64), decoupled from the
  // ring's SQ capacity (16). The pipeline fills the SQ, drains CQEs, and
  // refills — tracking arrays hold all ops regardless of ring size.
  SlotResult slots_[BATCH_MAX_OPS];
  PendingOp pending_[BATCH_MAX_OPS];

  // Per-slot bounce buffer offset for registered I/O. -1 = unregistered.
  // Set during push_to_ring, used during record_cqe for memcpy + release.
  int32_t reg_off_[BATCH_MAX_OPS];

  RingState *ring_;
  ThreadRing *tr_;      // For reg_buf_acquire/release/ptr
  HANDLE event_;
  HANDLE file_;
  uint32_t gen_;
  uint32_t count_;      // Total ops registered (via push_read/write/flush)

  // Pipeline queue depth. At QD=8 with 64K unbuffered I/O, NVMe stays
  // continuously busy while CQEs are drained — 99% faster than batch-drain
  // (Research E3).
  static constexpr uint32_t PIPELINE_QD = 8;

  // Minimum SQEs to accumulate before calling submit() during refill.
  // Research E4 shows batch=2-4 is the sweet spot: submit syscall cost
  // amortizes from 78K/op at batch=1 to 41K/op at batch=4. We submit
  // when we've queued SUBMIT_BATCH replacements, or when we've exhausted
  // the pending queue (whichever comes first).
  static constexpr uint32_t SUBMIT_BATCH = 4;

  // Pipeline state: tracks which ops have been pushed to the ring vs
  // still pending, and how many are currently in flight.
  uint32_t next_push_;  // Next pending op to push into ring SQ
  uint32_t in_flight_;  // SQEs submitted to kernel but not yet completed

  // Registered buffer state.
  bool use_regbuf_;     // True if O_DIRECT + reg_buf_ready
  uint32_t reg_drain_;  // FIFO release cursor — lowest slot not yet released

public:
  /// Initialize for a new batch. Must be called before pushing ops.
  /// Advances the generation counter and pre-drains stale CQEs.
  ///
  /// When use_regbuf is true and the ThreadRing has a registered buffer,
  /// the pipeline uses MDL pre-pinned bounce buffers for read/write SQEs.
  /// This eliminates per-op page table walks — 67% faster for unbuffered
  /// I/O (Research D4). Ops that exceed available buffer space fall back
  /// to unregistered I/O transparently.
  LIBC_INLINE void init(ThreadRing *tr, HANDLE file,
                        bool use_regbuf = false) {
    tr_ = tr;
    ring_ = &tr->ring;
    event_ = tr->event;
    file_ = file;
    gen_ = tr->next_generation();
    count_ = 0;
    next_push_ = 0;
    in_flight_ = 0;
    use_regbuf_ = use_regbuf && tr->ensure_reg_buf();
    reg_drain_ = 0;

    // Pre-drain: discard stale CQEs from cancelled previous operations.
    // Guarantees CQ headroom (CQ_SIZE >= 2 * SQ_SIZE by construction).
    drain_stale_cqes(ring_);
  }

  /// Register a read op. Returns the slot index (0..N-1) or -1 if full.
  /// Does NOT push to the ring — the pipeline handles submission timing.
  [[nodiscard]] LIBC_INLINE int push_read(void *buf, ULONG len,
                                          ULONGLONG offset) {
    if (count_ >= BATCH_MAX_OPS)
      return -1;
    uint32_t slot = count_++;
    pending_[slot] = {buf, len, offset, PendingOp::READ, 0};
    slots_[slot] = {0, len, 0, false};
    return static_cast<int>(slot);
  }

  /// Register a write op. Returns the slot index or -1 if full.
  [[nodiscard]] LIBC_INLINE int push_write(const void *buf, ULONG len,
                                           ULONGLONG offset) {
    if (count_ >= BATCH_MAX_OPS)
      return -1;
    windows::prefault_read_pages(buf, len);
    uint32_t slot = count_++;
    pending_[slot] = {const_cast<void *>(buf), len, offset,
                      PendingOp::WRITE, 0};
    slots_[slot] = {0, len, 0, false};
    return static_cast<int>(slot);
  }

  /// Register a flush op. Returns the slot index or -1 if full.
  [[nodiscard]] LIBC_INLINE int push_flush(ULONG flush_mode =
                                               FLUSH_FLAGS_FILE_NORMAL) {
    if (count_ >= BATCH_MAX_OPS)
      return -1;
    uint32_t slot = count_++;
    pending_[slot] = {nullptr, 0, 0, PendingOp::FLUSH, flush_mode};
    slots_[slot] = {0, 0, 0, false};
    return static_cast<int>(slot);
  }

  /// Number of ops registered so far.
  [[nodiscard]] LIBC_INLINE uint32_t count() const { return count_; }

  /// Access per-slot result after drain. Caller must check slot < count().
  [[nodiscard]] LIBC_INLINE const SlotResult &result(uint32_t slot) const {
    return slots_[slot];
  }

  /// Submit all registered ops and drain all CQEs via pipeline.
  ///
  /// Pipeline strategy (Research E3):
  ///
  ///   Push up to PIPELINE_QD SQEs → submit → drain completed CQEs →
  ///   push replacements from pending queue → submit refill → repeat
  ///   until all ops are submitted and all CQEs are drained.
  ///
  ///   Key insight: never wait for count>1. The completion event fires
  ///   once per CQ empty→non-empty transition. Waiting for 1 CQE at a
  ///   time and draining greedily is the only correct event-based
  ///   strategy. For NVMe, CQEs are available on immediate poll (no
  ///   wait syscall needed). For slow I/O, the alertable event wait
  ///   provides EINTR delivery.
  ///
  ///   Small batches (count <= PIPELINE_QD): all ops pushed at once,
  ///   submit_wait lets the kernel coalesce completions. Same efficiency
  ///   as pre-pipeline batch-drain — one syscall, zero extra latency
  ///   for cached I/O.
  ///
  /// On EINTR: cancels in-flight SQEs (not pending ops that were never
  /// submitted), drains their CQEs, and returns partial progress.
  ///
  /// Returns BatchResult with the successful prefix length and total bytes.
  [[nodiscard]] LIBC_INLINE BatchResult submit_and_drain() {
    if (count_ == 0)
      return {0, 0, 0};

    // Install SyscallFrame for pthread_cancel / fork visibility.
    ThreadLifecycle *lc = get_current_lifecycle();
    OpTag base_tag{gen_, 0};
    signal_state::SyscallFrame frame{
        lc ? lc->active_syscall.load(cpp::MemoryOrder::RELAXED) : nullptr,
        ring_, file_, event_, base_tag.as_user_data(),
        /*batch_count=*/count_};
    if (lc)
      lc->active_syscall.store(&frame, cpp::MemoryOrder::RELEASE);
    auto unlink_frame = cpp::make_scope_guard([&] {
      if (lc)
        lc->active_syscall.store(frame.prev, cpp::MemoryOrder::RELEASE);
    });

    return pipeline();
  }

private:
  /// Push a single pending op into the ring's SQ. Returns true on success.
  ///
  /// When use_regbuf_ is active, tries to acquire a bounce buffer region
  /// for read/write SQEs. If the buffer is full, falls back to unregistered
  /// for this op and all subsequent ops (sets use_regbuf_ = false). This
  /// guarantees registered allocations are contiguous from slot 0, which
  /// is required for FIFO release via reg_drain_.
  LIBC_INLINE bool push_to_ring(uint32_t slot) {
    OpTag tag{gen_, slot};
    const auto &op = pending_[slot];
    NT_IORING_SQE *sqe = nullptr;
    reg_off_[slot] = -1;

    // Try registered buffer path for read/write ops.
    if (use_regbuf_ && op.type != PendingOp::FLUSH) {
      int32_t boff = tr_->reg_buf_acquire(op.len);
      if (boff >= 0) {
        reg_off_[slot] = boff;
        if (op.type == PendingOp::WRITE) {
          // Stage user data into bounce buffer before submission.
          __builtin_memcpy(tr_->reg_buf_ptr(static_cast<uint32_t>(boff)),
                           op.buf, op.len);
        }
        sqe = (op.type == PendingOp::READ)
                  ? ioring::push_read_regbuf(
                        ring_, file_, /*buf_index=*/0,
                        static_cast<ULONG>(boff), op.len, op.offset,
                        tag.as_user_data())
                  : ioring::push_write_regbuf(
                        ring_, file_, /*buf_index=*/0,
                        static_cast<ULONG>(boff), op.len, op.offset,
                        tag.as_user_data());
        if (sqe)
          return true;
        // SQ full with registered op — release and fail.
        tr_->reg_buf_release(op.len);
        reg_off_[slot] = -1;
        return false;
      }
      // Bounce buffer exhausted — switch to unregistered for remaining ops.
      // This preserves FIFO release ordering: all registered ops are
      // contiguous from slot 0.
      use_regbuf_ = false;
    }

    // Unregistered path (raw buffer pointer, no bounce).
    switch (op.type) {
    case PendingOp::READ:
      sqe = ioring::push_read(ring_, file_, op.buf, op.len, op.offset,
                              tag.as_user_data());
      break;
    case PendingOp::WRITE:
      sqe = ioring::push_write(ring_, file_, op.buf, op.len, op.offset,
                               tag.as_user_data());
      break;
    case PendingOp::FLUSH:
      sqe = ioring::push_flush(ring_, file_, tag.as_user_data(),
                               op.flush_mode);
      break;
    }
    return sqe != nullptr;
  }

  /// Pipeline submit+drain core. Implements the E3 pattern:
  /// fill QD → submit → drain → refill → wait for 1 → repeat.
  [[nodiscard]] LIBC_INLINE BatchResult pipeline() {
    uint32_t drained = 0;

    // Ensure all registered buffer regions are released on any exit path.
    auto regbuf_cleanup = cpp::make_scope_guard([&] { release_all_regbufs(); });

    // ── Push all ops into the ring and submit with wait ──
    // Push everything upfront — SQ capacity (64) exceeds any reasonable
    // chunk count. Use submit(ring_, count_) to block until all CQEs
    // are ready in a single syscall. This avoids the event-based
    // pipeline path which has known issues with cached I/O (kernel
    // posts CQEs synchronously, completion event never fires).
    for (uint32_t i = 0; i < count_; ++i) {
      if (!push_to_ring(next_push_))
        break;
      ++next_push_;
      ++in_flight_;
    }
    if (in_flight_ == 0)
      return {0, EIO, 0};

    {
      NTSTATUS s = ioring::submit(ring_, in_flight_);
      if (!NT_SUCCESS(s))
        return {0, EIO, 0};
    }

    // ── Pipeline loop ──
    //
    // Tracks unflushed SQEs: replacements pushed to the ring SQ but not
    // yet submitted via NtSubmitIoRing. We batch these to amortize the
    // submit syscall cost (E4: 78K/op at batch=1 → 41K/op at batch=4).
    uint32_t unflushed = 0;

    while (drained < count_) {
      // Phase 1: Drain all immediately available CQEs (no syscall).
      // pop_cqe_gen internally consumes stale CQEs, advancing CQ.Head
      // past them — after this loop the CQ is truly empty if we got
      // no match, allowing the completion event to re-fire correctly.
      while (drained < count_) {
        NT_IORING_CQE cqe;
        if (!pop_cqe_gen(ring_, gen_, &cqe))
          break;
        record_cqe(cqe);
        ++drained;
        --in_flight_;
      }

      // Release completed registered buffer regions in FIFO order.
      // This frees bounce buffer space for refill (push_to_ring may
      // acquire new regions for subsequent registered ops).
      try_release_regbufs();

      if (drained >= count_)
        break;

      // Phase 2: Refill — push replacements to keep the device busy.
      while (next_push_ < count_ && in_flight_ < PIPELINE_QD) {
        if (!push_to_ring(next_push_))
          break;
        ++next_push_;
        ++in_flight_;
        ++unflushed;
      }

      // Submit when we've batched enough replacements, or when we've
      // exhausted the pending queue and have unflushed SQEs to send.
      if (unflushed >= SUBMIT_BATCH ||
          (unflushed > 0 && next_push_ >= count_)) {
        ioring::submit(ring_); // nowait — start new IRPs immediately
        unflushed = 0;
      }

      // Flush any unflushed SQEs before draining/parking.
      if (unflushed > 0) {
        ioring::submit(ring_);
        unflushed = 0;
      }

      // Phase 2.5: Re-drain after submit. For cached I/O, CQEs are
      // posted synchronously during NtSubmitIoRing — the completion
      // event never fires (no async DPC). Must check CQ before waiting.
      {
        bool found_any = false;
        while (drained < count_) {
          NT_IORING_CQE cqe;
          if (!pop_cqe_gen(ring_, gen_, &cqe))
            break;
          record_cqe(cqe);
          ++drained;
          --in_flight_;
          found_any = true;
        }
        try_release_regbufs();
        if (found_any)
          continue; // Loop back — more ops may need refill/drain.
        if (drained >= count_)
          break;
      }

      // Phase 3: Wait for at least 1 CQE (alertable for EINTR).
      // Only reached for async I/O (O_DIRECT, NVMe) where CQEs
      // arrive via completion DPC after NtSubmitIoRing returns.
      // Flush CQ head so the kernel sees free CQ slots.
      ioring::flush_cq_head(ring_);

      NTSTATUS ws =
          ::NtWaitForSingleObject(event_, /*Alertable=*/1, /*Timeout=*/nullptr);

      if (ws == STATUS_USER_APC) {
        if (signal_state::should_restart_syscall())
          continue; // SA_RESTART — keep draining.

        // EINTR — cancel only in-flight SQEs (already submitted to
        // kernel). Pending ops that haven't been pushed are simply
        // not submitted — no cancel needed.
        cancel_in_flight();
        drain_remaining_after_cancel(drained);
        return compute_result(EINTR);
      }

      if (ws != STATUS_SUCCESS)
        return compute_result(EIO);
      // STATUS_SUCCESS → event fired, CQE arrived. Loop back to drain.
    }

    return compute_result(0);
  }

  /// Record a CQE's result into the matching slot. For registered reads,
  /// copies data from the bounce buffer to the caller's buffer.
  LIBC_INLINE void record_cqe(const NT_IORING_CQE &cqe) {
    auto tag = OpTag::from_user_data(cqe.UserData);
    if (tag.is_cancel() || tag.slot >= count_)
      return; // Cancel CQE or out-of-range — ignore.

    auto &slot = slots_[tag.slot];
    slot.completed = true;
    if (is_eof_result(cqe.ResultCode)) {
      slot.bytes = 0;
      slot.error = 0;
    } else if (cqe.ResultCode < 0) {
      slot.error = cqe_status_to_errno(cqe.ResultCode);
    } else {
      slot.bytes = static_cast<size_t>(cqe.Information);
    }

    // Registered read: copy from bounce buffer to the caller's buffer.
    // Write ops staged their data pre-submit — no post-completion copy.
    int32_t boff = reg_off_[tag.slot];
    if (boff >= 0 && pending_[tag.slot].type == PendingOp::READ &&
        slot.bytes > 0) {
      __builtin_memcpy(pending_[tag.slot].buf,
                       tr_->reg_buf_ptr(static_cast<uint32_t>(boff)),
                       slot.bytes);
    }
  }

  /// FIFO release of registered buffer regions. Advances reg_drain_
  /// through contiguous completed slots, releasing their bounce buffer
  /// regions in allocation order. The linear buffer model requires FIFO
  /// release — out-of-order release would corrupt head/tail accounting.
  LIBC_INLINE void try_release_regbufs() {
    while (reg_drain_ < next_push_ && reg_off_[reg_drain_] >= 0 &&
           slots_[reg_drain_].completed) {
      tr_->reg_buf_release(pending_[reg_drain_].len);
      reg_off_[reg_drain_] = -1;
      ++reg_drain_;
    }
    // Skip past unregistered slots (reg_off_ == -1) that are completed,
    // so reg_drain_ stays aligned with the oldest unreleased registered op.
    while (reg_drain_ < next_push_ && reg_off_[reg_drain_] < 0 &&
           slots_[reg_drain_].completed) {
      ++reg_drain_;
    }
  }

  /// Release all outstanding registered buffer regions. Called on exit
  /// (normal or EINTR) to ensure no bounce buffer leaks. Walks slots
  /// from reg_drain_ forward in FIFO order.
  LIBC_INLINE void release_all_regbufs() {
    for (uint32_t i = reg_drain_; i < next_push_; ++i) {
      if (reg_off_[i] >= 0) {
        tr_->reg_buf_release(pending_[i].len);
        reg_off_[i] = -1;
      }
    }
  }

  /// Cancel all in-flight SQEs — ops that have been pushed to the ring
  /// and submitted but haven't completed yet. Only cancels slots in the
  /// range [0, next_push_) that aren't already completed. Pending ops
  /// (slot >= next_push_) were never submitted and need no cancel.
  LIBC_INLINE void cancel_in_flight() {
    for (uint32_t i = 0; i < next_push_; ++i) {
      if (slots_[i].completed)
        continue;
      OpTag slot_tag{gen_, i};
      auto cancel_tag = OpTag::make_cancel(gen_);
      auto *sqe = ioring::push_cancel(ring_, file_,
                                       slot_tag.as_user_data(),
                                       cancel_tag.as_user_data());
      if (!sqe) {
        // SQ full — flush what we have and retry this slot.
        ioring::submit(ring_);
        ioring::push_cancel(ring_, file_, slot_tag.as_user_data(),
                            cancel_tag.as_user_data());
      }
    }
    ioring::submit(ring_);
  }

  /// After cancel: drain up to (remaining + cancel count) CQEs with timeout.
  /// Bound: each in-flight SQE produces at most 1 original CQE + 1 cancel
  /// CQE = 2 CQEs per slot. The 3x + 8 bound provides generous headroom
  /// for stale entries that pop_cqe_gen skips internally.
  LIBC_INLINE void drain_remaining_after_cancel(uint32_t &drained) {
    uint32_t max_attempts = next_push_ * 3 + 8;
    for (unsigned attempts = 0; attempts < max_attempts; ++attempts) {
      NT_IORING_CQE cqe;
      if (pop_cqe_gen(ring_, gen_, &cqe)) {
        auto tag = OpTag::from_user_data(cqe.UserData);
        if (!tag.is_cancel() && tag.slot < count_) {
          record_cqe(cqe);
          ++drained;
        }
        continue;
      }
      // Brief wait for stragglers.
      LARGE_INTEGER timeout;
      timeout.QuadPart = -50LL * 10000LL; // 50ms
      NTSTATUS ws = ::NtWaitForSingleObject(event_, /*Alertable=*/0, &timeout);
      if (ws == STATUS_TIMEOUT)
        break; // Good enough — remaining stale CQEs will be rejected by
               // generation check on next operation.
    }
  }

  /// Walk slots in submission order, sum the successful prefix.
  ///
  /// POSIX semantics: return total bytes from the longest prefix of
  /// completed slots with no errors and no short transfers. A slot
  /// past the first short-read/error has wrong offsets (pre-computed
  /// assuming full transfers) — its result is discarded.
  [[nodiscard]] LIBC_INLINE BatchResult compute_result(int drain_error) const {
    size_t total = 0;
    uint32_t prefix = 0;

    for (uint32_t i = 0; i < count_; ++i) {
      const auto &slot = slots_[i];

      // Slot never completed (EINTR cancelled it, or never submitted
      // because the pipeline hadn't reached it yet) — stop.
      if (!slot.completed)
        break;

      // Error on this slot — stop, report the error.
      if (slot.error) {
        if (prefix == 0)
          return {0, slot.error, 0};
        break; // Partial transfer — return bytes, not error.
      }

      total += slot.bytes;
      ++prefix;

      // Short transfer — stop (subsequent offsets are wrong).
      if (slot.requested > 0 && slot.bytes < slot.requested)
        break;
    }

    // If we drained everything but got interrupted, report EINTR only
    // if no bytes were transferred.
    if (drain_error == EINTR && prefix == 0)
      return {0, EINTR, 0};

    return {total, 0, prefix};
  }
};

} // namespace ioring
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_BATCH_ENGINE_H
