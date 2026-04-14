//===-- Implementation header of fsetpos -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_FSETPOS_H
#define LLVM_LIBC_SRC_STDIO_FSETPOS_H

#include "hdr/types/FILE.h"
#include "hdr/types/fpos_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int fsetpos(::FILE *__restrict stream, const fpos_t *__restrict pos);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_FSETPOS_H
