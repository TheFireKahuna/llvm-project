//===-- Socket engine forward declarations --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal socket kernel functions — return ssize_t for data transfer or long
// for other ops (value on success, -errno on failure). Entry points in
// src/sys/socket/windows/ are thin wrappers that translate to POSIX return
// conventions (libc_errno + -1).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SOCKET_ENGINE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SOCKET_ENGINE_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/ssize_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "hdr/types/struct_msghdr.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// --- Socket lifecycle ---
intptr_t socket(int domain, int type, int protocol);
intptr_t bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
intptr_t listen(int sockfd, int backlog);
intptr_t connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
intptr_t accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
intptr_t shutdown(int sockfd, int how);
intptr_t socketpair(int domain, int type, int protocol, int sv[2]);

// --- Data transfer ---
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
              struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);

// --- Socket queries ---
intptr_t getsockopt(int sockfd, int level, int optname, void *optval,
                socklen_t *optlen);
intptr_t setsockopt(int sockfd, int level, int optname, const void *optval,
                socklen_t optlen);
intptr_t getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
intptr_t getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SOCKET_ENGINE_H
