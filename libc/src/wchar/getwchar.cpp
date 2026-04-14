//===-- Implementation of getwchar ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/getwchar.h"

#include "hdr/types/wint_t.h"
#include "src/__support/File/file.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/wchar/fgetwc.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, getwchar, ()) {
  return LIBC_NAMESPACE::fgetwc(reinterpret_cast<::FILE *>(stdin));
}

} // namespace LIBC_NAMESPACE_DECL
