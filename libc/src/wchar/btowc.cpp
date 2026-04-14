//===-- Implementation of btowc -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/btowc.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/mbrtowc.h"
#include "src/__support/wchar/mbstate.h"

#include "hdr/types/wint_t.h"
#include "hdr/wchar_macros.h" // for WEOF.

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, btowc, (int c)) {
  if (c < 0 || c > 255)
    return WEOF;

  // btowc(c) is equivalent to mbtowc(&wc, &buf, 1) per C11 §7.29.6.1.1.
  char buf = static_cast<char>(static_cast<unsigned char>(c));
  wchar_t wc;
  internal::mbstate state;
  auto ret = internal::mbrtowc(&wc, &buf, 1, &state);
  if (!ret.has_value() || ret.value() == static_cast<size_t>(-2))
    return WEOF;
  return static_cast<wint_t>(wc);
}

} // namespace LIBC_NAMESPACE_DECL
