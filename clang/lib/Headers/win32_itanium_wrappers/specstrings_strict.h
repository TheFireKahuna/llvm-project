/*===---- specstrings_strict.h - Windows SDK annotation wrapper ------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_SPECSTRINGS_STRICT_H
#define __CLANG_SPECSTRINGS_STRICT_H

/* The SDK's specstrings_strict.h defines __null as an annotation. Keep clang's
 * __null, which stddef.h's NULL expands to in C++. */
#pragma push_macro("__null")
#include_next <specstrings_strict.h>
#pragma pop_macro("__null")

#endif /* __CLANG_SPECSTRINGS_STRICT_H */
