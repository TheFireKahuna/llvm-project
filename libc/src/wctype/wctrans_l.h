//===-- Implementation header for wctrans_l ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_WCTYPE_WCTRANS_L_H
#define LLVM_LIBC_SRC_WCTYPE_WCTRANS_L_H

#include "hdr/types/locale_t.h"
#include "hdr/types/wctrans_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

wctrans_t wctrans_l(const char *charclass, locale_t locale);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_WCTYPE_WCTRANS_L_H
