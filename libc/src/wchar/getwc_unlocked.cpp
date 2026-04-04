//===-- Implementation of getwc_unlocked ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/getwc_unlocked.h"

#include "hdr/types/FILE.h"
#include "hdr/types/wint_t.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/wchar/fgetwc_unlocked.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, getwc_unlocked, (::FILE * stream)) {
  return LIBC_NAMESPACE::fgetwc_unlocked(stream);
}

} // namespace LIBC_NAMESPACE_DECL
