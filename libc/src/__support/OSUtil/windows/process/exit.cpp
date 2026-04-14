//===-- Windows implementation of an exit function ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/runtime.h"

#if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
#include "src/__support/OSUtil/windows/ntdll.h"
#else
// On Win32 we cannot make direct syscalls since Microsoft changes system call
// IDs periodically. We must rely on functions exported from ntdll.dll or
// kernel32.dll to invoke system service procedures.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace LIBC_NAMESPACE_DECL {
namespace internal {

[[noreturn]] void exit(int status) {
  #if defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
  // Use NtTerminateProcess directly to bypass DLL detach handlers that may
  // deadlock during process teardown. ExitProcess → LdrShutdownProcess can
  // hang if an FLS or DLL_PROCESS_DETACH callback blocks.
  ::NtTerminateProcess(reinterpret_cast<HANDLE>(-1), status);
  __builtin_unreachable();
  #else
  ::ExitProcess(status);
  #endif
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
