/*===---- new.h - UCRT new.h wrapper for Windows Itanium ------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 *
 * UCRT new.h includes <crtdefs.h> when _MSC_EXTENSIONS is defined. On Windows
 * Itanium / libc++ builds, there is no MSVC runtime and <crtdefs.h> is not
 * available. This wrapper forces the !_MSC_EXTENSIONS path for those builds
 * while still including the real UCRT new.h to get its C declarations.
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_UCRT_NEW_H
#define __CLANG_UCRT_NEW_H

/* The SDK owns the C new-handler and allocation-mode declarations. Keep its
 * optional Microsoft C++ declarations out of the Itanium ABI. */
#if defined(_WIN32_ITANIUM)
#pragma push_macro("_MSC_EXTENSIONS")
#undef _MSC_EXTENSIONS
#endif
#if __has_include_next(<new.h>)
#include_next <new.h>
#endif
#if defined(_WIN32_ITANIUM)
#pragma pop_macro("_MSC_EXTENSIONS")
#endif

#endif /* __CLANG_UCRT_NEW_H */
