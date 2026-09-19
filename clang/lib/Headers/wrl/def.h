/*===---- wrl/def.h - Windows Runtime C++ Template Library wrapper --------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_wrl_def_h
#define __clang_wrl_def_h

/*
 * The SDK's wrl/def.h refuses any compiler whose _MSC_VER is below 16.00,
 * which an undefined _MSC_VER also fails. Everything else in WRL compiles
 * and runs as written. _MSC_VER is set only across that one header, after
 * the SDK headers it includes are already in, so no other header sees it.
 */
#if defined(_WIN32_ITANIUM) && !defined(_MSC_VER) && __has_include_next(<wrl/def.h>)
#include <sdkddkver.h>
#include <sal.h>
#pragma push_macro("_MSC_VER")
#define _MSC_VER 1933
#include_next <wrl/def.h>
#pragma pop_macro("_MSC_VER")
#elif __has_include_next(<wrl/def.h>)
#include_next <wrl/def.h>
#endif

#endif /* __clang_wrl_def_h */
