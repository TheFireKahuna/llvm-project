/*===---- vcruntime_startup.h - Start-up argument mode ---------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The UCRT's corecrt_startup.h, which process.h includes, takes the type of
 * its argv configuration functions' argument from the Visual C++ runtime's
 * header. */

#ifndef __CLANG_VCRUNTIME_STARTUP_H
#define __CLANG_VCRUNTIME_STARTUP_H

#include <vcruntime.h>

_CRT_BEGIN_C_HEADER

typedef enum _crt_argv_mode {
  _crt_argv_no_arguments,
  _crt_argv_unexpanded_arguments,
  _crt_argv_expanded_arguments,
} _crt_argv_mode;

_CRT_END_C_HEADER

#endif /* __CLANG_VCRUNTIME_STARTUP_H */
