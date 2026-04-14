//===-- Implementation of wctob -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wctob.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/mbstate.h"
#include "src/__support/wchar/wcrtomb.h"

#include "hdr/stdio_macros.h" // for EOF.
#include "hdr/types/wint_t.h"
#include "hdr/wchar_macros.h" // for WEOF.

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, wctob, (wint_t c)) {
  if (c == WEOF)
    return EOF;

  // wctob(c) is the inverse of btowc: return the single-byte encoding of c,
  // or EOF if c doesn't correspond to a single-byte character.
  char buf[4];
  internal::mbstate state;
  auto ret = internal::wcrtomb(buf, static_cast<wchar_t>(c), &state);
  if (!ret.has_value() || ret.value() != 1)
    return EOF;
  return static_cast<int>(static_cast<unsigned char>(buf[0]));
}

} // namespace LIBC_NAMESPACE_DECL
