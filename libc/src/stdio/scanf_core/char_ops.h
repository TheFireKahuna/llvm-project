//===-- Char-type traits for scanf pipeline ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Dispatch helper for bracket-set membership testing. Narrow (`bitset<256>`)
// and wide (`WideScanSet`) expose `test(c)` with different argument types and
// indexing conventions; in_scanset() hides that. Section type is no longer
// selected here — it is `basic_format_section<CharT>` directly from
// core_structs.h.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_SCANF_CORE_CHAR_OPS_H
#define LLVM_LIBC_SRC_STDIO_SCANF_CORE_CHAR_OPS_H

#include "hdr/types/wchar_t.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/stdio/scanf_core/core_structs.h"

namespace LIBC_NAMESPACE_DECL {
namespace scanf_core {

template <typename CharT> struct CharOps;

template <> struct CharOps<char> {
  // O(1) bitset probe for narrow %[ membership.
  LIBC_INLINE static bool in_scanset(const basic_format_section<char> &sec,
                                     char c) {
    return sec.scan_set.test(static_cast<unsigned char>(c));
  }
};

template <> struct CharOps<wchar_t> {
  // Range-list scan for wide %[ membership.
  LIBC_INLINE static bool in_scanset(const basic_format_section<wchar_t> &sec,
                                     wchar_t c) {
    return sec.scan_set.test(c);
  }
};

} // namespace scanf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_SCANF_CORE_CHAR_OPS_H
