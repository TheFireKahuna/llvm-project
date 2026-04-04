//===-- PCB-resident PTY tree state --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_TREE_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_TREE_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/threads/raw_mutex.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct PtyTreeState {
  RawMutex lock;
  cpp::Atomic<uint32_t> initialized;
  uint8_t tree_nonce[16];
  void *boundary_descriptor;
  HANDLE namespace_handle;
  HANDLE alloc_lock;
  HANDLE alloc_section;
  void *alloc_state_view;
  HANDLE service_port;
  uint64_t service_create_time;
  HANDLE tree_job;
  uint16_t inherited_attach_flags;
  uint16_t reserved0;
  cpp::Atomic<uint32_t> current_attached_pty_id;
  HANDLE inherited_reference;
  HANDLE current_state_lock;
  HANDLE current_state_section;
  void *current_state_view;
};

static_assert(__is_trivially_destructible(PtyTreeState),
              "PtyTreeState must stay trivially destructible for the PCB");

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_TREE_STATE_H
