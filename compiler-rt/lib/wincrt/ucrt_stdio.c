//===-- ucrt_stdio.c - Universal CRT stdio options ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Universal CRT's stdio headers define __local_stdio_printf_options and
// __local_stdio_scanf_options inline, each returning the image's own option
// word. C++ emits them as COMDATs, but C emits no definition of an inline
// function, so a C image needs these. They are weak, so a C++ object's
// definition takes precedence over them in whatever order the linker sees
// the two. An option word of zero selects the ISO C behaviour.
//
// The headers also make every object that selects ISO wide specifiers name
// a symbol that only a Visual C++ library defines; it is defined here.
//
//===----------------------------------------------------------------------===//

__attribute__((weak)) unsigned long long *__local_stdio_printf_options(void) {
  static unsigned long long Options;
  return &Options;
}

__attribute__((weak)) unsigned long long *__local_stdio_scanf_options(void) {
  static unsigned long long Options;
  return &Options;
}

// The name contains dots, so it is given as the assembly name.
__attribute__((used)) const char __wincrt_iso_stdio_wide_specifiers __asm__(
    "__PLEASE_LINK_WITH_iso_stdio_wide_specifiers.lib") = 0;
