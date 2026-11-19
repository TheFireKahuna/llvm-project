/*===---- wrl/def.h - Windows Runtime C++ Template Library wrapper ---------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_WRL_DEF_H
#define __CLANG_WRL_DEF_H

/* The SDK's wrl/def.h stops with an error unless _MSC_VER is at least 1600,
 * and the rest of the library does not test it. Define _MSC_VER across that
 * header only, after the headers it includes. */
#include <sal.h>
#include <sdkddkver.h>
#pragma push_macro("_MSC_VER")
#ifndef _MSC_VER
#define _MSC_VER 1933
#endif
#include_next <wrl/def.h>
#pragma pop_macro("_MSC_VER")

#endif /* __CLANG_WRL_DEF_H */
