//===-- Implementation of strerror_r --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/string/strerror_r.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/StringUtil/error_to_string.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/string/memory_utils/inline_memcpy.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {

// XSI-compliant strerror_r: returns 0 on success, or ERANGE if buf is too
// small. The message is always NUL-terminated when buflen > 0.
LLVM_LIBC_FUNCTION(int, strerror_r, (int err_num, char *buf, size_t buflen)) {
  cpp::string_view msg = get_error_string(err_num);
  if (msg.size() >= buflen) {
    if (buflen > 0) {
      inline_memcpy(buf, msg.data(), buflen - 1);
      buf[buflen - 1] = '\0';
    }
    return ERANGE;
  }
  inline_memcpy(buf, msg.data(), msg.size() + 1);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
