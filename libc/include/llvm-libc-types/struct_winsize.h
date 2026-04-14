//===-- Definition of struct winsize -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef __LLVM_LIBC_TYPES_STRUCT_WINSIZE_H__
#define __LLVM_LIBC_TYPES_STRUCT_WINSIZE_H__

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

#endif // __LLVM_LIBC_TYPES_STRUCT_WINSIZE_H__
