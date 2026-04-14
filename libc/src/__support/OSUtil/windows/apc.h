//===-- Native APC helpers for Windows ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One level above raw NtQueueApcThreadEx2, one level below subsystem policy.
// These helpers intentionally expose the native three-argument APC ABI and
// return raw NTSTATUS results. libc-internal users should prefer this layer
// over QueueUserAPC2 when they do not need Win32 callback adaptation or
// activation-context replay.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_APC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_APC_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

inline constexpr ULONG APC_FLAGS_NONE = 0x00000000;
inline constexpr ULONG APC_FLAGS_SPECIAL_USER = 0x00000001;

// CALLBACK_DATA_CONTEXT flag (Windows 11+). Only effective when the APC
// routine is RtlDispatchAPC (the Win32 QueueUserAPC2 intermediary).
// We don't need this flag — KiUserApcDispatcher already passes the
// interrupted CONTEXT* as a hidden 4th argument (r9 on x64, x3 on ARM64)
// to every APC routine, regardless of flags. This is the mechanism that
// RtlDispatchAPC itself uses internally.
inline constexpr ULONG APC_FLAGS_CALLBACK_DATA_CONTEXT = 0x00010000;

LIBC_INLINE NTSTATUS queue_user_apc(HANDLE thread, PPS_APC_ROUTINE routine,
                                    PVOID arg1 = nullptr, PVOID arg2 = nullptr,
                                    PVOID arg3 = nullptr,
                                    ULONG flags = APC_FLAGS_NONE) {
  return ::NtQueueApcThreadEx2(thread, nullptr, flags, routine, arg1, arg2,
                               arg3);
}

// Queue a special user APC. This does not wake the target thread from
// NtWaitForAlertByThreadId or other blocking waits; callers that need prompt
// delivery should pair it with NtAlertThreadByThreadId/NtAlertMultiple*.
LIBC_INLINE NTSTATUS
queue_special_user_apc(HANDLE thread, PPS_APC_ROUTINE routine,
                       PVOID arg1 = nullptr, PVOID arg2 = nullptr,
                       PVOID arg3 = nullptr) {
  return queue_user_apc(thread, routine, arg1, arg2, arg3,
                        APC_FLAGS_SPECIAL_USER);
}

// Queue a signal-delivery APC as a special user APC.
// KiUserApcDispatcher passes the interrupted thread's CONTEXT* as a hidden
// 4th argument (r9 on x64, x3 on ARM64) to every APC routine. The callback
// simply declares a 4th parameter to receive it — no naked stubs or stack
// frame probing required.
LIBC_INLINE NTSTATUS
queue_signal_apc(HANDLE thread, PPS_APC_ROUTINE routine,
                 PVOID arg1 = nullptr, PVOID arg2 = nullptr,
                 PVOID arg3 = nullptr) {
  return queue_user_apc(thread, routine, arg1, arg2, arg3,
                        APC_FLAGS_SPECIAL_USER);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_APC_H
