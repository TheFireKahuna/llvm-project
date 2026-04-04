//===-- SyscallFrame for blocking syscall interruptibility --------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stack-allocated frame installed by blocking syscalls around alertable waits.
// Enables pthread_cancel and fork to identify and cancel in-flight I/O.
//
// Frames form a stack via the `prev` pointer — signal handlers that call
// blocking syscalls push nested frames automatically:
//
//   read() installs frame A → handler fires → write() installs frame B
//   active_syscall → B (prev → A)
//   write() returns → active_syscall → A (prev → whatever was before)
//
// Install pattern (2 lines each way):
//
//   auto *lc = get_current_lifecycle();
//   SyscallFrame frame{lc->active_syscall.load(RELAXED), ring, handle, event,
//                      tag.as_user_data()};
//   lc->active_syscall.store(&frame, RELEASE);
//   // ... alertable wait ...
//   lc->active_syscall.store(frame.prev, RELEASE);
//
// active_syscall is Atomic<SyscallFrame *> for consistency with other
// cross-thread-visible fields in ThreadLifecycle. Current readers always
// suspend the target first, but Atomic prevents compiler reordering.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SIGNAL_WINDOWS_SYSCALL_FRAME_H
#define LLVM_LIBC_SRC_SIGNAL_WINDOWS_SYSCALL_FRAME_H

#include "src/__support/OSUtil/windows/io/op_tag.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

struct SyscallFrame {
  SyscallFrame *prev;       // Saved outer frame (nested syscalls in handlers)
  ioring::RingState *ring;  // OFD's embedded ring (null = sync path)
  HANDLE file_handle;       // File handle for NtCancelIoFileEx
  HANDLE event;             // Completion event (IO Ring path)
  ULONGLONG op_tag;         // Generation-tagged UserData (OpTag::as_user_data)
  uint32_t batch_count;     // 0 = single-op, >0 = batch (N SQEs in flight)
};

// Cancel all in-flight I/O on a SyscallFrame chain. Walks prev pointers.
// Called by cancel_act (async pthread_cancel) and fork child reinit.
// Each frame is cancelled at most once — the caller clears active_syscall
// after this returns.
//
// Uses generation-tagged cancel SQEs. Stale CQEs from prior generations
// are silently skipped during drain — no timeout-based heuristics needed.
LIBC_INLINE void cancel_inflight_io(SyscallFrame *frame) {
  for (; frame; frame = frame->prev) {
    if (frame->ring) {
      auto tag = ioring::OpTag::from_user_data(frame->op_tag);
      uint32_t gen = tag.generation;

      if (frame->batch_count > 0) {
        // Batch cancellation: push a cancel for each outstanding SQE.
        // Slot indices 0..batch_count-1 were used for the batch.
        for (uint32_t i = 0; i < frame->batch_count; ++i) {
          ioring::OpTag slot_tag{gen, i};
          auto cancel_tag = ioring::OpTag::make_cancel(gen);
          ioring::push_cancel(frame->ring, frame->file_handle,
                              slot_tag.as_user_data(),
                              cancel_tag.as_user_data());
        }
      } else {
        // Single-op cancellation.
        auto cancel_tag = ioring::OpTag::make_cancel(gen);
        ioring::push_cancel(frame->ring, frame->file_handle,
                            frame->op_tag, cancel_tag.as_user_data());
      }
      ioring::submit(frame->ring);

      // Drain CQEs with generation filtering — skip stale entries from
      // prior generations, count only matching ones.
      unsigned expected =
          (frame->batch_count > 0 ? frame->batch_count : 1u) + 1u;
      unsigned drained [[maybe_unused]] = 0;
      NT_IORING_CQE cqe;
      for (int attempts = 0; attempts < static_cast<int>(expected + 4);
           ++attempts) {
        if (ioring::pop_cqe(frame->ring, &cqe)) {
          auto cqe_tag = ioring::OpTag::from_user_data(cqe.UserData);
          if (cqe_tag.is_generation(gen))
            ++drained;
          // Stale CQEs from other generations are silently discarded.
          continue;
        }
        if (!frame->event)
          break;
        LARGE_INTEGER timeout;
        timeout.QuadPart = -100LL * 10000LL; // 100ms
        if (::NtWaitForSingleObject(frame->event, 0, &timeout) ==
            STATUS_TIMEOUT)
          break;
      }
    } else if (frame->file_handle) {
      // Sync alertable path: cancel via NtCancelIoFileEx.
      IO_STATUS_BLOCK iosb = {};
      ::NtCancelIoFileEx(frame->file_handle, nullptr, &iosb);
    }
  }
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SIGNAL_WINDOWS_SYSCALL_FRAME_H
