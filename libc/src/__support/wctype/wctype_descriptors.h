//===-- Shared wctype/wctrans descriptor constants --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_WCTYPE_WCTYPE_DESCRIPTORS_H
#define LLVM_LIBC_SRC___SUPPORT_WCTYPE_WCTYPE_DESCRIPTORS_H

#include "hdr/types/wctype_t.h"
#include "hdr/types/wctrans_t.h"

namespace LIBC_NAMESPACE_DECL {

// Classification descriptors returned by wctype(). POSIX requires all 12
// standard classes. Zero means invalid.
static constexpr wctype_t WCTYPE_ALNUM = 1;
static constexpr wctype_t WCTYPE_ALPHA = 2;
static constexpr wctype_t WCTYPE_BLANK = 3;
static constexpr wctype_t WCTYPE_CNTRL = 4;
static constexpr wctype_t WCTYPE_DIGIT = 5;
static constexpr wctype_t WCTYPE_GRAPH = 6;
static constexpr wctype_t WCTYPE_LOWER = 7;
static constexpr wctype_t WCTYPE_PRINT = 8;
static constexpr wctype_t WCTYPE_PUNCT = 9;
static constexpr wctype_t WCTYPE_SPACE = 10;
static constexpr wctype_t WCTYPE_UPPER = 11;
static constexpr wctype_t WCTYPE_XDIGIT = 12;

// Transformation descriptors returned by wctrans(). POSIX requires
// "toupper" and "tolower". Zero means invalid.
static constexpr wctrans_t WCTRANS_TOUPPER = 1;
static constexpr wctrans_t WCTRANS_TOLOWER = 2;

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_WCTYPE_WCTYPE_DESCRIPTORS_H
