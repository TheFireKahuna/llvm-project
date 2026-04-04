//===-- Process-wide state for the ALPC bus ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Carved out of `signal/process_signal_state.h` when the signal transport
// was generalized into the multiplexed ALPC bus. Lives in `g_pcb` because
// several subsystems need read-only access to the bus's identity fields
// (e.g. signal delivery puts `self_create_time` into its outbound payload).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_ALPC_BUS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_ALPC_BUS_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Process-wide observable state for the ALPC bus. Transport-private
// fields (SD buffers, ACL heap, reactor token) stay file-local inside
// alpc_bus.cpp — only what other subsystems legitimately read lives here.
struct AlpcBusState {
  HANDLE   namespace_handle; // Private namespace shared across same-user libc processes.
  HANDLE   port;             // This process's receive port (renamed on fork).
  void    *boundary;         // OBJECT_BOUNDARY_DESCRIPTOR* for the namespace.
  uint64_t create_time;      // This process's NtQueryInformationProcess(Times).CreateTime.
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_ALPC_BUS_STATE_H
