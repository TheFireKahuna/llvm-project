//===-- ucrt.c - Universal CRT names that no DLL exports ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Visual C++ supplies these from its static libraries.
//
//===----------------------------------------------------------------------===//

// math.h's XENIX name of HUGE_VAL, and its traditional name, which is weak so
// that a program's own HUGE takes precedence.
const double _HUGE = __builtin_huge_val();
__attribute__((weak)) double HUGE = __builtin_huge_val();

// Every object that selects ISO wide specifiers in stdio.h names this. The
// name contains dots, so it is given as the assembly name.
const char __wincrt_iso_stdio_wide_specifiers __asm__(
    "__PLEASE_LINK_WITH_iso_stdio_wide_specifiers.lib") = 0;
