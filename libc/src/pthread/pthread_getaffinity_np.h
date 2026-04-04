//===-- Implementation header for pthread_getaffinity_np --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_PTHREAD_PTHREAD_GETAFFINITY_NP_H
#define LLVM_LIBC_SRC_PTHREAD_PTHREAD_GETAFFINITY_NP_H

#include "hdr/types/cpu_set_t.h"
#include "hdr/types/size_t.h"
#include "src/__support/macros/config.h"
#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

int pthread_getaffinity_np(pthread_t th, size_t cpusetsize,
                           cpu_set_t *cpuset);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_PTHREAD_PTHREAD_GETAFFINITY_NP_H
