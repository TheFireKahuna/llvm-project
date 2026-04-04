//===-- Process identity state for Windows ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PROCESS_IDENTITY_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PROCESS_IDENTITY_STATE_H

#include "hdr/types/gid_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_identity {

struct ProcessIdentityState {
  cpp::Atomic<pid_t> pgid;
  cpp::Atomic<uint32_t> pgid_initialized;
  cpp::Atomic<uid_t> real_uid;
  cpp::Atomic<uid_t> eff_uid;
  cpp::Atomic<uid_t> saved_uid;
  cpp::Atomic<gid_t> real_gid;
  cpp::Atomic<gid_t> eff_gid;
  cpp::Atomic<gid_t> saved_gid;
  cpp::Atomic<uint8_t> privilege_level;
  cpp::Atomic<HANDLE> impersonation_token;
  cpp::Atomic<uint32_t> initialized;
  cpp::Atomic<unsigned> umask;
};

} // namespace windows_identity
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PROCESS_IDENTITY_STATE_H
