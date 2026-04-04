//===-- Internal implementation header of vfwscanf --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_VFWSCANF_INTERNAL_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_VFWSCANF_INTERNAL_H

#include "src/__support/File/file.h"
#include "src/__support/arg_list.h"
#include "src/__support/macros/config.h"
#include "src/stdio/scanf_core/scanf_main.h"
#include "src/stdio/scanf_core/wide_stream_reader.h"

#include "hdr/types/FILE.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

LIBC_INLINE int vfwscanf_internal(::FILE *__restrict stream,
                                  const wchar_t *__restrict format,
                                  internal::ArgList &args) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();
  // Adopt wide orientation; reject on byte-oriented streams
  // (C11 §7.21.2p3/p4).
  if (!f->adopt_orientation_unlocked(1)) {
    f->set_err_unlocked();
    f->unlock();
    return EOF;
  }

  WideStreamReader reader(f);
  int retval = scanf_core::scanf_main(&reader, format, args);

  // Match vfscanf: a successful scan with zero counted assignments (%n,
  // assignment suppression) must still return 0 even if the final lookahead
  // hit EOF. Only convert zero to EOF when an actual read error occurred.
  if (retval == 0 && f->error_unlocked())
    retval = EOF;
  f->unlock();
  return retval;
}

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_VFWSCANF_INTERNAL_H
