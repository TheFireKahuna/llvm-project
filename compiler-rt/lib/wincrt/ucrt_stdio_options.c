//===-- ucrt_stdio_options.c - UCRT per-image stdio option storage --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// UCRT's stdio headers define __local_stdio_printf_options and
// __local_stdio_scanf_options as noinline __inline functions returning
// per-image option storage. C++ translation units emit COMDAT definitions; in
// C, clang gives __inline GNU89 semantics and emits no definition at all, so a
// C-only image references undefined symbols. These definitions satisfy that
// case. A C++ object in the same image supplies the COMDAT first and this
// archive member is never extracted, which is why nothing else may live in
// this translation unit. Zero storage selects ISO wide-format conversions and
// standard snprintf behaviour, the state the driver's
// _CRT_STDIO_ISO_WIDE_SPECIFIERS requests.
//
//===----------------------------------------------------------------------===//

unsigned long long *__local_stdio_printf_options(void) {
  static unsigned long long Options;
  return &Options;
}

unsigned long long *__local_stdio_scanf_options(void) {
  static unsigned long long Options;
  return &Options;
}
