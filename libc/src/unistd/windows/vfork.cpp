//===-- Windows implementation of vfork ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// vfork() on NT-POSIX == fork(). POSIX explicitly permits vfork to behave
// identically to fork, and Linux glibc itself has long done this on several
// architectures. We route straight through the naked-asm __llvm_libc_sys_fork
// engine so the caller gets the same CoW clone semantics as plain fork().
//
// The historical vfork contract (child shares parent's address space, parent
// blocks until child exec/exit) is explicitly no longer required by POSIX
// and is outright dangerous on a CoW clone substrate — don't emulate it.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/vfork.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include "hdr/stdint_proxy.h"

namespace LIBC_NAMESPACE_DECL {

// Engine symbol emitted by src/unistd/windows/fork.cpp. Re-declared here
// so vfork has its own translation-unit link closure with no source-file
// coupling beyond the resolved symbol.
namespace internal {
intptr_t sys_fork(void) asm("__llvm_libc_sys_fork");
} // namespace internal

LLVM_LIBC_FUNCTION(pid_t, vfork, (void)) {
  intptr_t ret = internal::sys_fork();
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  return static_cast<pid_t>(ret);
}

} // namespace LIBC_NAMESPACE_DECL
