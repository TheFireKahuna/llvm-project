//===-- Internal implementation header of vfwprintf --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_PRINTF_CORE_VFWPRINTF_INTERNAL_H
#define LLVM_LIBC_SRC_STDIO_PRINTF_CORE_VFWPRINTF_INTERNAL_H

#include "hdr/types/FILE.h"
#include "hdr/types/wchar_t.h"
#include "src/__support/File/file.h"
#include "src/__support/arg_list.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/stdio/printf_core/wide_stream_writer.h"
#include "src/stdio/printf_core/wprintf_main.h"

namespace LIBC_NAMESPACE_DECL {
namespace printf_core {

LIBC_INLINE ErrorOr<size_t>
vfwprintf_internal(::FILE *__restrict stream,
                   const wchar_t *__restrict format,
                   internal::ArgList &args) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Set wide orientation (C11 §7.21.2p4).
  f->fwide_unlocked(1);

  WideStreamWriter writer(f);

  auto retval = wprintf_main(&writer, format, args);
  if (!retval.has_value()) {
    f->unlock();
    return retval;
  }

  // Flush buffered byte output from the stream.
  int flushval = f->flush_unlocked();
  if (flushval != 0) {
    f->unlock();
    return Error(flushval);
  }

  f->unlock();
  return retval;
}

} // namespace printf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_PRINTF_CORE_VFWPRINTF_INTERNAL_H
