//===-- POSIX header sys/un.h ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SYS_UN_H
#define LLVM_LIBC_SYS_UN_H

// POSIX: sys/un.h provides struct sockaddr_un.
// Our sys/socket.h already defines it.
#include <sys/socket.h>

#endif // LLVM_LIBC_SYS_UN_H
