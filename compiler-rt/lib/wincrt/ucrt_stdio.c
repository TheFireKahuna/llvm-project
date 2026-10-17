//===-- ucrt_stdio.c - Universal CRT stdio wide-specifier marker ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Universal CRT's stdio headers make every object that selects ISO wide
// specifiers name a symbol that only a Visual C++ library defines; it is
// defined here.
//
//===----------------------------------------------------------------------===//

// The name contains dots, so it is given as the assembly name.
__attribute__((used)) const char __wincrt_iso_stdio_wide_specifiers __asm__(
    "__PLEASE_LINK_WITH_iso_stdio_wide_specifiers.lib") = 0;
