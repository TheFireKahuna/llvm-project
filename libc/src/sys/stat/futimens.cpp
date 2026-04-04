//===-- Implementation of futimens ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/futimens.h"

#include "hdr/fcntl_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/sys/stat/utimensat.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, futimens,
                   (int fd, const struct timespec times[2])) {
  return LIBC_NAMESPACE::utimensat(fd, "", times, AT_EMPTY_PATH);
}

} // namespace LIBC_NAMESPACE_DECL
