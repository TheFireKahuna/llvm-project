//===-- Internal epoll engine declarations -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: epoll engine functions that implement
// Linux epoll semantics on Windows via IOCP.
// Return long: non-negative value on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_EPOLL_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_EPOLL_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

struct epoll_event;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t epoll_create1(int flags);
intptr_t epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
intptr_t epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                int timeout);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_EPOLL_OPS_H
