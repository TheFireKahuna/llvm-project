//===-- Implementation of strerror_l --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/string/strerror_l.h"
#include "src/__support/StringUtil/error_to_string.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include "hdr/types/locale_t.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, strerror_l, (int err_num, locale_t)) {
  return const_cast<char *>(get_error_string(err_num).data());
}

} // namespace LIBC_NAMESPACE_DECL
