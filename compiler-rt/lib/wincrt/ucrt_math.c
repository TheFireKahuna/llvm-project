//===-- ucrt_math.c - Universal CRT math data -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Universal CRT's math.h declares _HUGE, the XENIX name of HUGE_VAL, and
// its traditional name HUGE, but ucrtbase.dll exports neither: Visual C++
// supplies them from its static libraries. HUGE is weak, so that a program's
// own HUGE takes precedence.
//
//===----------------------------------------------------------------------===//

const double _HUGE = __builtin_huge_val();

__attribute__((weak)) double HUGE = __builtin_huge_val();
