//===-- UNICODE_STRING initialization helpers -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers for initialising NT UNICODE_STRING descriptors from runtime buffers.
// The compile-time template form for static arrays lives in ipc/condrv.h.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_UNICODE_STRING_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_UNICODE_STRING_UTILS_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_util {

// Initialise a UNICODE_STRING from a mutable buffer.
// char_count is the number of WCHARs EXCLUDING the NUL terminator.
// MaximumLength is set to (char_count + 1) * sizeof(WCHAR) to account
// for the NUL.  No overflow checking — use init_unicode_string_safe for
// untrusted lengths.
LIBC_INLINE void init_unicode_string(UNICODE_STRING *us, WCHAR *buffer,
                                     size_t char_count) {
  us->Length = static_cast<USHORT>(char_count * sizeof(WCHAR));
  us->MaximumLength = static_cast<USHORT>((char_count + 1) * sizeof(WCHAR));
  us->Buffer = buffer;
}

// Initialise a UNICODE_STRING from a const source with an overflow guard.
// Returns false if the resulting byte length would exceed the USHORT limit
// (0xFFFC = 65534, the maximum UNICODE_STRING Length).
LIBC_INLINE bool init_unicode_string_safe(UNICODE_STRING *dest,
                                          const WCHAR *src,
                                          size_t char_count) {
  size_t bytes = char_count * sizeof(WCHAR);
  if (bytes > 0xFFFC)
    return false;
  dest->Length = static_cast<USHORT>(bytes);
  dest->MaximumLength = static_cast<USHORT>(bytes + sizeof(WCHAR));
  // NT's UNICODE_STRING::Buffer is typed as non-const PWSTR even for
  // read-only use (e.g., NtCreateFile ObjectName). The const_cast is safe
  // here because we never mutate through this pointer — NT APIs that modify
  // the buffer use a separate output UNICODE_STRING.
  dest->Buffer = const_cast<WCHAR *>(src);
  return true;
}

} // namespace windows_util
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_UNICODE_STRING_UTILS_H
