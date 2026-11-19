/*===---- sal.h - Windows SDK source annotation wrapper --------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_SAL_H
#define __CLANG_SAL_H

/* The SDK's sal.h defines __null as an annotation. Keep clang's __null, which
 * stddef.h's NULL expands to in C++. */
#pragma push_macro("__null")
#include_next <sal.h>
#pragma pop_macro("__null")

#endif /* __CLANG_SAL_H */
