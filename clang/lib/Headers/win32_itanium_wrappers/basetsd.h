/*===---- basetsd.h - Windows SDK basetsd.h wrapper ------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_BASETSD_H
#define __CLANG_BASETSD_H

#ifndef __cplusplus
/* The SDK defines its pointer conversion helpers __inline without extern.
 * Outside the Microsoft C++ ABI such a C definition is only an inline
 * definition, so a call that is not inlined refers to a function that no
 * library defines. Give them internal linkage instead. */
#pragma push_macro("__inline")
#define __inline static __inline
#include_next <basetsd.h>
#pragma pop_macro("__inline")
#else
#include_next <basetsd.h>
#endif

#endif /* __CLANG_BASETSD_H */
