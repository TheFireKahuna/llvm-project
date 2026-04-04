//===-- Pipeline IoRing batch engine ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// BatchEngineT accumulates N I/O operations and submits them through a
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
// ────────────────────────────────────────────────────────────────────────────
// Traits-based configuration
// ────────────────────────────────────────────────────────────────────────────
//
// Pipeline tunables are expressed as a traits struct (DefaultBatchCfg) so
// benches can instantiate BatchEngineT<CustomCfg> to A/B test alternative
// strategies without duplicating any logic. The default alias BatchEngine
// (= BatchEngineT<DefaultBatchCfg>) preserves the historical hard-coded
// behavior for production callers. Explicit template instantiation for the
// default Cfg lives in thread_ring.cpp.
//
// ────────────────────────────────────────────────────────────────────────────
// Split public API
// ────────────────────────────────────────────────────────────────────────────
//
// submit_and_drain() composes the default E3 pipeline. For benches and
// custom strategies, the phases are also exposed as inlinable primitives:
//
//   fill_ring(up_to)      — push up to N pending ops into SQ, stop on SQ full
//   flush_submit(wait)    — NtSubmitIoRing over unflushed SQEs
//   drain_available()     — pop all immediately-available matching CQEs
//   wait_one<I>()         — block until ≥1 CQE available (I = interruptible)
//   cancel_in_flight()    — cancel SQEs submitted but not yet completed
//   drain_after_cancel()  — bounded drain of cancel CQEs post-cancellation
//   compute_result()      — walk slots in submission order, find prefix
//
// Benches can assemble arbitrary strategies (batch-drain, pipeline@QD=N,
// poll-only, wait-count>1, mixed) by composing these. All are LIBC_INLINE
// so there's zero call overhead vs the monolithic submit_and_drain path.
//
// Design constraints:
//
//   No fixed-size arrays — result tracking is bounded by Cfg::MAX_OPS
//   (decoupled from ring SQ capacity). The per-slot byte count array and
//   completion bitmap are sized to Cfg::MAX_OPS at compile time. The
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
// BatchResult — returned by BatchEngineT::submit_and_drain()
//===----------------------------------------------------------------------===//

struct BatchResult {
  size_t total_bytes;   // Sum of bytes across the successful prefix
  int error;            // 0 on full success, errno on first failure or EINTR
  uint32_t completed;   // Number of ops in the successful prefix
};

//===----------------------------------------------------------------------===//
// DefaultBatchCfg — production tunables
//===----------------------------------------------------------------------===//
//
// MAX_OPS (64): maximum ops per batch — tracking array size. Decoupled
// from the ring's SQ capacity (16). The pipeline fills the SQ, drains
// CQEs, and refills, so batches larger than SQ are handled transparently.
//
// PIPELINE_QD (8): in-flight target during steady-state refill. Research
// E3: QD=8 with 64K unbuffered I/O keeps NVMe continuously busy while
// CQEs are drained. 99% faster than batch-drain.
//
// SUBMIT_BATCH (4): minimum SQEs to accumulate before calling submit()
// during refill. Research E4: batch=2-4 is the sweet spot — submit syscall
// cost amortizes from 78K/op at batch=1 to 41K/op at batch=4.
//
// INITIAL_QD (UINT32_MAX): cap on the initial fill before first submit.
// Default UINT32_MAX means "push until SQ-full or count_ exhausted" —
// preserves historical behavior (push up to ring SQ capacity, typically
// 16). Set to PIPELINE_QD for uniform pipeline depth across the whole
// batch (no asymmetric initial burst).
//
// INITIAL_WAIT (true): if true, the first submit is submit_wait(in_flight_),
// blocking in-kernel until all initially-pushed CQEs arrive. Optimal for
// cached I/O (kernel posts CQEs synchronously during NtSubmitIoRing, so
// submit_wait returns immediately — single-syscall). Anti-pattern for
// async I/O where it forfeits pipeline parallelism on the initial chunk.
// Set to false for async-heavy workloads: submit_nowait + drain_available
// inside the pipeline loop picks up cached CQEs just as efficiently and
// lets async CQEs arrive under steady pipeline depth.
//
// The initial-submit policy is orthogonal to the steady-state pipeline:
// (INITIAL_QD, INITIAL_WAIT) controls the first NtSubmitIoRing call, and
// (PIPELINE_QD, SUBMIT_BATCH) controls refill pacing afterward.

struct DefaultBatchCfg {
  static constexpr uint32_t MAX_OPS = 64;
  static constexpr uint32_t PIPELINE_QD = 8;
  static constexpr uint32_t SUBMIT_BATCH = 4;
  static constexpr uint32_t INITIAL_QD = 0xFFFFFFFFu; // uncapped — SQ bounds it
  static constexpr bool INITIAL_WAIT = true;          // submit_wait(in_flight_)
};

//===----------------------------------------------------------------------===//
// BatchEngineT
//===----------------------------------------------------------------------===//

template <class Cfg = DefaultBatchCfg>
class BatchEngineT {
public:
  // Re-export traits for callers bounded on MAX_OPS (iov collector loops).
  static constexpr uint32_t MAX_OPS = Cfg::MAX_OPS;
  static constexpr uint32_t PIPELINE_QD = Cfg::PIPELINE_QD;
  static constexpr uint32_t SUBMIT_BATCH = Cfg::SUBMIT_BATCH;
  static constexpr uint32_t INITIAL_QD = Cfg::INITIAL_QD;
  static constexpr bool INITIAL_WAIT = Cfg::INITIAL_WAIT;

  // Result tracking: one entry per registered op.
  struct SlotResult {
    size_t bytes;       // CQE.Information (bytes transferred)
    size_t requested;   // Bytes requested in this SQE (for short-read detect)
    int error;          // 0 on success, errno on failure
    bool completed;     // CQE received for this slot
  };

private:
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

  // Tracking arrays hold all ops regardless of ring SQ size.
  SlotResult slots_[MAX_OPS];
  PendingOp pending_[MAX_OPS];

  // Per-slot bounce buffer offset for registered I/O. -1 = unregistered.
  // Set during push_to_ring, used during record_cqe for memcpy + release.
  int32_t reg_off_[MAX_OPS];

  RingState *ring_;
  ThreadRing *tr_;      // For reg_buf_acquire/release/ptr
  HANDLE event_;
  HANDLE file_;
  uint32_t gen_;
  uint32_t count_;      // Total ops registered (via push_read/write/flush)

  // Pipeline state: tracks which ops have been pushed to the ring vs
  // still pending, and how many are currently in flight.
  uint32_t next_push_;  // Next pending op to push into ring SQ
  uint32_t in_flight_;  // SQEs submitted to kernel but not yet completed

  // Replacements pushed to the ring SQ but not yet submitted via
  // NtSubmitIoRing. Tracked as a member (not local) so the split primitives
  // can share state with submit_and_drain().
  uint32_t unflushed_;

  // Registered buffer state.
  bool use_regbuf_;     // True if O_DIRECT + reg_buf_ready
  uint32_t reg_drain_;  // FIFO release cursor — lowest slot not yet released

public:
  //===--------------------------------------------------------------------===//
  // Lifecycle & registration
  //===--------------------------------------------------------------------===//

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
    unflushed_ = 0;
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
    if (count_ >= MAX_OPS)
      return -1;
    uint32_t slot = count_++;
    pending_[slot] = {buf, len, offset, PendingOp::READ, 0};
    slots_[slot] = {0, len, 0, false};
    return static_cast<int>(slot);
  }

  /// Register a write op. Returns the slot index or -1 if full.
  [[nodiscard]] LIBC_INLINE int push_write(const void *buf, ULONG len,
                                           ULONGLONG offset) {
    if (count_ >= MAX_OPS)
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
    if (count_ >= MAX_OPS)
      return -1;
    uint32_t slot = count_++;
    pending_[slot] = {nullptr, 0, 0, PendingOp::FLUSH, flush_mode};
    slots_[slot] = {0, 0, 0, false};
    return static_cast<int>(slot);
  }

  //===--------------------------------------------------------------------===//
  // Accessors — state inspection for benches and callers
  //===--------------------------------------------------------------------===//

  /// Total ops registered so far.
  [[nodiscard]] LIBC_INLINE uint32_t count() const { return count_; }

  /// Number of ops already pushed into the ring's SQ.
  [[nodiscard]] LIBC_INLINE uint32_t pushed() const { return next_push_; }

  /// Number of SQEs submitted to the kernel but not yet completed.
  [[nodiscard]] LIBC_INLINE uint32_t in_flight() const { return in_flight_; }

  /// Number of SQEs written to the ring but not yet flushed to the kernel.
  [[nodiscard]] LIBC_INLINE uint32_t unflushed() const { return unflushed_; }

  /// Generation tag — benches can use this for manual CQE filtering.
  [[nodiscard]] LIBC_INLINE uint32_t generation() const { return gen_; }

  /// Access per-slot result after drain. Caller must check slot < count().
  [[nodiscard]] LIBC_INLINE const SlotResult &result(uint32_t slot) const {
    return slots_[slot];
  }

  //===--------------------------------------------------------------------===//
  // High-level composition — default E3 pipeline
  //===--------------------------------------------------------------------===//

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

  //===--------------------------------------------------------------------===//
  // Split primitives — compose custom strategies (benches, policy tests)
  //===--------------------------------------------------------------------===//

  /// Push up to `up_to` pending ops into the ring's SQ. Stops early if the
  /// ring SQ fills. Does NOT call NtSubmitIoRing — use flush_submit() to
  /// send the pushed SQEs to the kernel.
  ///
  /// To bound by an in-flight queue depth (QD), pass
  /// `min(count() - pushed(), QD - in_flight())`.
  ///
  /// Returns the number of ops pushed in this call.
  LIBC_INLINE uint32_t fill_ring(uint32_t up_to) {
    uint32_t pushed = 0;
    while (pushed < up_to && next_push_ < count_) {
      if (!push_to_ring(next_push_))
        break;
      ++next_push_;
      ++in_flight_;
      ++unflushed_;
      ++pushed;
    }
    return pushed;
  }

  /// Submit unflushed SQEs to the kernel.
  ///
  /// wait_count == 0: fire-and-forget (submit_nowait) — kernel starts
  ///                  processing SQEs; CQEs become available asynchronously.
  /// wait_count  > 0: submit_wait(N) — block in-kernel until N CQEs are
  ///                  available. Blocks the entire thread, so unsuitable
  ///                  when interruptibility is required. Best for small
  ///                  batches where all CQEs complete synchronously.
  ///
  /// Returns NT status; STATUS_SUCCESS on success.
  LIBC_INLINE NTSTATUS flush_submit(uint32_t wait_count = 0) {
    NTSTATUS s = ioring::submit(ring_, wait_count);
    if (NT_SUCCESS(s))
      unflushed_ = 0;
    return s;
  }

  /// Drain all immediately-available CQEs matching the current generation.
  /// Stale CQEs (wrong generation) are silently skipped. Does NOT block.
  /// Releases registered buffer regions in FIFO order as slots complete.
  /// Returns the number of CQEs drained in this call.
  LIBC_INLINE uint32_t drain_available() {
    uint32_t drained = 0;
    NT_IORING_CQE cqe;
    while (pop_cqe_gen(ring_, gen_, &cqe)) {
      record_cqe(cqe);
      ++drained;
      if (in_flight_ > 0)
        --in_flight_;
    }
    try_release_regbufs();
    return drained;
  }

  /// Block until at least one matching CQE is available. Does NOT drain —
  /// the caller must follow with drain_available() to consume CQEs.
  ///
  /// Strategy: flush_cq_head so the kernel sees free CQ slots, then
  /// ThreadLocalWord::wait_for_addr which performs the two-phase adaptive
  /// wait (UMWAIT/MWAITX on cq->Tail, then NtWaitForAlertByThreadId).
  /// wait_for_addr returns immediately if cq->Tail has already changed,
  /// eliminating the Dekker re-check burden from the caller.
  ///
  /// Interruptible=true: APC delivery returns -EINTR.
  /// Interruptible=false: medium-path wait (no EINTR).
  ///
  /// Returns 0 on wake, -EINTR on interruption.
  template <bool Interruptible = true>
  LIBC_INLINE long wait_one() {
    ioring::flush_cq_head(ring_);
    return tr_->cq_word.template wait_for_addr<Interruptible>(
        reinterpret_cast<volatile uint32_t *>(&ring_->cq->Tail),
        static_cast<uint32_t>(ring_->cq_tail_cache));
  }

  /// Cancel all in-flight SQEs — ops that have been pushed to the ring
  /// and submitted but haven't completed yet. Only cancels slots in the
  /// range [0, next_push_) that aren't already completed. Pending ops
  /// (slot >= next_push_) were never submitted and need no cancel.
  /// Issues NtSubmitIoRing internally to dispatch the cancel SQEs.
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
    unflushed_ = 0;
  }

  /// After cancel: drain up to (3·next_push_ + 8) CQEs with a short
  /// timeout. Each in-flight SQE produces at most 1 original CQE + 1
  /// cancel CQE. The 3x + 8 bound provides generous headroom for stale
  /// entries that pop_cqe_gen skips internally. Updates `drained` with
  /// any non-cancel CQEs that arrived post-cancellation.
  LIBC_INLINE void drain_after_cancel(uint32_t &drained) {
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
  ///
  /// Pass EINTR via drain_error if the batch was interrupted; returns
  /// EINTR only if no bytes were transferred (otherwise partial bytes).
  [[nodiscard]] LIBC_INLINE BatchResult
  compute_result(int drain_error = 0) const {
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

  /// Release all outstanding registered buffer regions. Called on exit
  /// (normal or EINTR) to ensure no bounce buffer leaks. Walks slots
  /// from reg_drain_ forward in FIFO order. Public so split-primitive
  /// users can call it from their own scope guards.
  LIBC_INLINE void release_all_regbufs() {
    for (uint32_t i = reg_drain_; i < next_push_; ++i) {
      if (reg_off_[i] >= 0) {
        tr_->reg_buf_release(pending_[i].len);
        reg_off_[i] = -1;
      }
    }
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

  /// Pipeline submit+drain core. Implements the E3 pattern by composing
  /// the split primitives: fill → submit_wait → drain → refill → wait →
  /// repeat. Kept here (private) so submit_and_drain retains a single
  /// canonical production path; benches compose the primitives directly.
  [[nodiscard]] LIBC_INLINE BatchResult pipeline() {
    uint32_t drained = 0;

    // Ensure all registered buffer regions are released on any exit path.
    auto regbuf_cleanup =
        cpp::make_scope_guard([&] { release_all_regbufs(); });

    // ── Initial fill + submit ──
    // Push up to INITIAL_QD ops (default UINT32_MAX → SQ-full bounds it,
    // typically 16). Then submit with INITIAL_WAIT policy:
    //
    //   INITIAL_WAIT=true (default): submit_wait(in_flight_) — kernel
    //     blocks until all initially-pushed CQEs arrive. Optimal for
    //     cached I/O (single syscall, CQEs posted synchronously) but
    //     forfeits pipeline parallelism on async I/O.
    //
    //   INITIAL_WAIT=false: submit_nowait — kernel starts SQEs and
    //     returns. Phase 1 drain_available below picks up synchronous
    //     (cached) CQEs. For async I/O, drain returns 0 and the pipeline
    //     loop handles refill + wait at uniform PIPELINE_QD.
    fill_ring(INITIAL_QD);
    if (in_flight_ == 0)
      return {0, EIO, 0};

    {
      NTSTATUS s = flush_submit(INITIAL_WAIT ? in_flight_ : 0);
      if (!NT_SUCCESS(s))
        return {0, EIO, 0};
    }

    // ── Pipeline loop ──
    while (drained < count_) {
      // Phase 1: Drain all immediately available CQEs (no syscall).
      drained += drain_available();
      if (drained >= count_)
        break;

      // Phase 2: Refill — push replacements to keep the device busy.
      if (next_push_ < count_ && in_flight_ < PIPELINE_QD) {
        uint32_t room = PIPELINE_QD - in_flight_;
        uint32_t want = count_ - next_push_;
        fill_ring(want < room ? want : room);
      }

      // Submit when we've batched enough replacements, or when we've
      // exhausted the pending queue and have unflushed SQEs to send.
      if (unflushed_ >= SUBMIT_BATCH ||
          (unflushed_ > 0 && next_push_ >= count_)) {
        flush_submit(0); // nowait — start new IRPs immediately
      }

      // Flush any unflushed SQEs before draining/parking.
      if (unflushed_ > 0)
        flush_submit(0);

      // Phase 2.5: Re-drain after submit. For cached I/O, CQEs are
      // posted synchronously during NtSubmitIoRing — the completion
      // event never fires (no async DPC). Must check CQ before waiting.
      {
        uint32_t more = drain_available();
        drained += more;
        if (more > 0)
          continue; // Loop back — more ops may need refill/drain.
        if (drained >= count_)
          break;
      }

      // Phase 3: Wait for at least 1 CQE via ThreadLocalWord (alertable).
      // Only reached for async I/O (O_DIRECT, NVMe) where CQEs
      // arrive via completion DPC after NtSubmitIoRing returns.
      long r = wait_one<true>();

      if (r == -EINTR) {
        if (signal_state::should_restart_syscall())
          continue; // SA_RESTART — keep draining.

        // EINTR — cancel only in-flight SQEs (already submitted to
        // kernel). Pending ops that haven't been pushed are simply
        // not submitted — no cancel needed.
        cancel_in_flight();
        drain_after_cancel(drained);
        return compute_result(EINTR);
      }
      // r == 0: reactor signaled or cq->Tail changed. Loop back to drain.
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
};

//===----------------------------------------------------------------------===//
// Default alias + explicit instantiation declaration
//===----------------------------------------------------------------------===//
//
// BatchEngine is the production type — BatchEngineT<DefaultBatchCfg>.
// The explicit instantiation lives in thread_ring.cpp. All member
// functions are LIBC_INLINE (always_inline), so the instantiation is
// ODR-safe and produces no addressable copies at call sites — it exists
// for tooling/debugging and to document the canonical instantiation.

using BatchEngine = BatchEngineT<DefaultBatchCfg>;

extern template class BatchEngineT<DefaultBatchCfg>;

} // namespace ioring
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_BATCH_ENGINE_H
