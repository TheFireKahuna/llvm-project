//===--- Syscall dispatch-target aggregation anchor -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// No-op header that exists solely to anchor the `syscall_dispatch_targets`
// CMake header-library. Its DEPENDS list enumerates every subsystem the
// runtime `syscall_impl` switch can route to — the set of internal::* and
// signal_state::* helpers referenced out-of-line from syscall.h.
//
// The public `libc.src.unistd.syscall` and `libc.src.unistd.__llvm_libc_syscall`
// entrypoints DEPEND on that aggregate so their link closure pulls the
// dispatch targets in freestanding-unit test builds, where there is no
// c.dll/c.lib providing the full NT personality.
//
// Kept separate from `.syscall` (the header-only interface library) because
// `.syscall` forward-declares its callees to break fd_table/mapping_table/
// clock subsystem dependency cycles.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_DISPATCH_TARGETS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_DISPATCH_TARGETS_H

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_DISPATCH_TARGETS_H
