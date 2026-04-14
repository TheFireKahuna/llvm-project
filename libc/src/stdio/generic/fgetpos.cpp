//===-- Implementation of fgetpos -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdio/fgetpos.h"
#include "src/__support/File/file.h"

#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, fgetpos,
                   (::FILE *__restrict stream, fpos_t *__restrict pos)) {
  auto result = reinterpret_cast<LIBC_NAMESPACE::File *>(stream)->tell();
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  *pos = result.value();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
