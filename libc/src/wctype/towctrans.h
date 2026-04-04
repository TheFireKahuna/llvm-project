//===-- Implementation header for towctrans ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_WCTYPE_TOWCTRANS_H
#define LLVM_LIBC_SRC_WCTYPE_TOWCTRANS_H

#include "hdr/types/wctrans_t.h"
#include "hdr/types/wint_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

wint_t towctrans(wint_t c, wctrans_t desc);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_WCTYPE_TOWCTRANS_H
