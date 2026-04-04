//===-- Shared stderr helpers for signal printing ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SIGNAL_SIGNAL_PRINT_UTILS_H
#define LLVM_LIBC_SRC_SIGNAL_SIGNAL_PRINT_UTILS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/File/file.h"
#include "src/__support/integer_to_string.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/stdio/stderr.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

class SignalTextWriter {
  LIBC_NAMESPACE::File *file;
  int write_error = 0;

public:
  explicit SignalTextWriter(::FILE *stream)
      : file(reinterpret_cast<LIBC_NAMESPACE::File *>(stream)) {
    file->lock();
  }

  SignalTextWriter(const SignalTextWriter &) = delete;
  SignalTextWriter &operator=(const SignalTextWriter &) = delete;

  ~SignalTextWriter() { file->unlock(); }

  LIBC_INLINE int error() const { return write_error; }

  LIBC_INLINE void write(cpp::string_view str) {
    if (write_error != 0 || str.empty())
      return;

    auto result = file->write_unlocked(str.data(), str.size());
    if (result.has_error())
      write_error = result.error;
  }

  LIBC_INLINE void write_trimmed(cpp::string_view str) {
    if (!str.empty() && str.back() == '\0')
      str.remove_suffix(1);
    write(str);
  }

  LIBC_INLINE void write(const char *str) { write(cpp::string_view(str)); }

  template <typename T> LIBC_INLINE void write_dec(T value) {
    const IntegerToString<T> buffer(value);
    write(buffer.view());
  }

  LIBC_INLINE void write_hex_uintptr(uintptr_t value) {
    const IntegerToString<uintptr_t, radix::Hex::WithPrefix> buffer(value);
    write(buffer.view());
  }

  LIBC_INLINE void write_ptr(const void *ptr) {
    write_hex_uintptr(reinterpret_cast<uintptr_t>(ptr));
  }

  LIBC_INLINE void finish() const {
    if (write_error != 0)
      libc_errno = write_error;
  }
};

LIBC_INLINE void write_message_prefix(SignalTextWriter &writer,
                                      const char *message) {
  if (message != nullptr && message[0] != '\0') {
    writer.write(message);
    writer.write(": ");
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SIGNAL_SIGNAL_PRINT_UTILS_H
