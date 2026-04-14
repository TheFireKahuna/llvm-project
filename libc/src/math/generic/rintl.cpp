//===-- Implementation of rintl function ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/math/rintl.h"
#include "src/__support/FPUtil/FEnvImpl.h"
#include "src/__support/FPUtil/NearestIntegerOperations.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"


namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(long double, rintl, (long double x)) {
  long double result = fputil::round_using_current_rounding_mode(x);
  if (LIBC_UNLIKELY(result != x) && !fputil::FPBits<long double>(x).is_nan())
    fputil::raise_except_if_required(FE_INEXACT);
  return result;
}

} // namespace LIBC_NAMESPACE_DECL
