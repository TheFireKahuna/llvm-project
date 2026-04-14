//===-- Implementation of gmtime_s (MSVC-compatible) ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/gmtime_s.h"
#include "hdr/types/struct_tm.h"
#include "hdr/types/time_t.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/libc_errno.h"
#include "src/time/time_utils.h"

namespace LIBC_NAMESPACE_DECL {

// MSVC-compatible gmtime_s: arguments reversed from C11 Annex K.
LLVM_LIBC_FUNCTION(int, gmtime_s, (struct tm *result, const time_t *timer)) {
  if (!result || !timer) {
    libc_errno = EINVAL;
    return EINVAL;
  }
  struct tm *ret = time_utils::gmtime_internal(timer, result);
  if (!ret) {
    libc_errno = EINVAL;
    return EINVAL;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
