//===-- Implementation of wcsxfrm -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/wcsxfrm.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include "hdr/types/size_t.h"
#include "hdr/types/wchar_t.h"

namespace LIBC_NAMESPACE_DECL {

// Identity transform: wcscoll currently uses ordinal comparison
// (C-locale semantics), so no transformation is needed — wcscmp on
// the copy produces the same ordering as wcscoll on the original.
LLVM_LIBC_FUNCTION(size_t, wcsxfrm,
                   (wchar_t *__restrict dest, const wchar_t *__restrict src,
                    size_t n)) {
  size_t len = 0;
  while (src[len] != L'\0')
    ++len;
  if (n > len) {
    for (size_t i = 0; i <= len; ++i)
      dest[i] = src[i];
  }
  return len;
}

} // namespace LIBC_NAMESPACE_DECL
