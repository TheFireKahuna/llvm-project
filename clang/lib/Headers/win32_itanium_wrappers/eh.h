/*===---- eh.h - Termination handlers ---------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The part of the Visual C++ runtime's eh.h that the C++ runtime implements on
 * Windows Itanium: the termination handlers. The C++ runtime has no structured
 * exception translator, so _set_se_translator is not declared. */

#ifndef __CLANG_EH_H
#define __CLANG_EH_H

#ifndef __cplusplus
#error "eh.h is only for C++"
#endif

#include <exception>

using std::get_terminate;
using std::set_terminate;
using std::terminate;
using std::terminate_handler;

#endif /* __CLANG_EH_H */
