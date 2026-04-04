//===-- Implementation of wctype ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wctype/wctype.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wctype/wctype_descriptors.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wctype_t, wctype, (const char *property)) {
  if (__builtin_strcmp(property, "alnum") == 0)
    return WCTYPE_ALNUM;
  if (__builtin_strcmp(property, "alpha") == 0)
    return WCTYPE_ALPHA;
  if (__builtin_strcmp(property, "blank") == 0)
    return WCTYPE_BLANK;
  if (__builtin_strcmp(property, "cntrl") == 0)
    return WCTYPE_CNTRL;
  if (__builtin_strcmp(property, "digit") == 0)
    return WCTYPE_DIGIT;
  if (__builtin_strcmp(property, "graph") == 0)
    return WCTYPE_GRAPH;
  if (__builtin_strcmp(property, "lower") == 0)
    return WCTYPE_LOWER;
  if (__builtin_strcmp(property, "print") == 0)
    return WCTYPE_PRINT;
  if (__builtin_strcmp(property, "punct") == 0)
    return WCTYPE_PUNCT;
  if (__builtin_strcmp(property, "space") == 0)
    return WCTYPE_SPACE;
  if (__builtin_strcmp(property, "upper") == 0)
    return WCTYPE_UPPER;
  if (__builtin_strcmp(property, "xdigit") == 0)
    return WCTYPE_XDIGIT;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
