//===-- POSIX langinfo macros ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_LANGINFO_MACROS_H
#define LLVM_LIBC_MACROS_LANGINFO_MACROS_H

// POSIX nl_item constants for nl_langinfo().

// LC_CTYPE
#define CODESET 14

// LC_NUMERIC
#define RADIXCHAR 0
#define THOUSEP 1
#define DECIMAL_POINT RADIXCHAR
#define THOUSANDS_SEP THOUSEP

// LC_TIME
#define D_T_FMT 2
#define D_FMT 3
#define T_FMT 4
#define T_FMT_AMPM 5
#define AM_STR 6
#define PM_STR 7

#define DAY_1 8
#define DAY_2 9
#define DAY_3 10
#define DAY_4 11
#define DAY_5 12
#define DAY_6 13
#define DAY_7 14

// Note: DAY_7 and CODESET share value 14 in some implementations.
// We use distinct values by shifting CODESET.
#undef CODESET
#define CODESET 49

#define ABDAY_1 15
#define ABDAY_2 16
#define ABDAY_3 17
#define ABDAY_4 18
#define ABDAY_5 19
#define ABDAY_6 20
#define ABDAY_7 21

#define MON_1 22
#define MON_2 23
#define MON_3 24
#define MON_4 25
#define MON_5 26
#define MON_6 27
#define MON_7 28
#define MON_8 29
#define MON_9 30
#define MON_10 31
#define MON_11 32
#define MON_12 33

#define ABMON_1 34
#define ABMON_2 35
#define ABMON_3 36
#define ABMON_4 37
#define ABMON_5 38
#define ABMON_6 39
#define ABMON_7 40
#define ABMON_8 41
#define ABMON_9 42
#define ABMON_10 43
#define ABMON_11 44
#define ABMON_12 45

// LC_MESSAGES
#define YESEXPR 46
#define NOEXPR 47

// LC_MONETARY
#define CRNCYSTR 48
#define CURRENCY_SYMBOL CRNCYSTR

// LC_COLLATE — no nl_langinfo items defined by POSIX.

// ERA and ALT_DIGITS (optional POSIX extensions)
#define ERA 50
#define ERA_D_FMT 51
#define ERA_D_T_FMT 52
#define ERA_T_FMT 53
#define ALT_DIGITS 54

#endif // LLVM_LIBC_MACROS_LANGINFO_MACROS_H
