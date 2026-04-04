//===-- Windows implementation header for pselect ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The shared pselect.h includes <sys/select.h> which doesn't exist on
// Windows without a full runtimes build. This header provides the
// declaration directly using our own type definitions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SYS_SELECT_WINDOWS_PSELECT_H
#define LLVM_LIBC_SRC_SYS_SELECT_WINDOWS_PSELECT_H

#include "hdr/types/sigset_t.h"
#include "hdr/types/struct_timespec.h"
#include "include/llvm-libc-macros/sys-select-macros.h"
#include "include/llvm-libc-types/fd_set.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int pselect(int nfds, fd_set *__restrict read_set,
            fd_set *__restrict write_set, fd_set *__restrict error_set,
            const struct timespec *__restrict timeout,
            const sigset_t *__restrict sigmask);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SYS_SELECT_WINDOWS_PSELECT_H
