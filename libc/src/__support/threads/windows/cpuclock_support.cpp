//===-- Windows thread CPU clock support -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/cpuclock_support.h"

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/time/clock_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace cpuclock_support {

int get_thread_cpuclockid(int tid, clockid_t *clock_id) {
  if (tid <= 0 || clock_id == nullptr)
    return EINVAL;

  const uint32_t target_tid = static_cast<uint32_t>(tid);
  if (target_tid == static_cast<uint32_t>(::NtCurrentThreadId())) {
    *clock_id = CLOCK_THREAD_CPUTIME_ID;
    return 0;
  }

  CLIENT_ID cid{nullptr,
                reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(target_tid))};
  windows::ScopedNtHandle handle;
  NTSTATUS status =
      ::NtOpenThread(handle.put(), THREAD_QUERY_INFORMATION, nullptr, &cid);
  if (!NT_SUCCESS(status))
    return ESRCH;

  *clock_id = internal::make_thread_cpuclockid(target_tid);
  return 0;
}

} // namespace cpuclock_support
} // namespace LIBC_NAMESPACE_DECL
