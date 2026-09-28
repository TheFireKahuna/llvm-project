/*===---- vcruntime_new_debug.h - Debug allocation functions ---------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The UCRT's crtdbg.h and new.h include this header for the allocation
 * functions that take a block type, file name and line number, which
 * crtdbg.h's _CRTDBG_MAP_ALLOC uses. Windows Itanium has no debug runtime, so
 * they forward to the ordinary allocation functions. */

#ifndef __CLANG_VCRUNTIME_NEW_DEBUG_H
#define __CLANG_VCRUNTIME_NEW_DEBUG_H

#include <vcruntime.h>
#include <vcruntime_new.h>

#ifdef __cplusplus

#if __cplusplus >= 201103L
#define __CLANG_NEW_DEBUG_NOEXCEPT noexcept
#else
#define __CLANG_NEW_DEBUG_NOEXCEPT throw()
#endif

inline void *__CRTDECL operator new(size_t _Size, int /*_BlockUse*/,
                                    char const * /*_FileName*/,
                                    int /*_LineNumber*/) {
  return ::operator new(_Size);
}

inline void *__CRTDECL operator new[](size_t _Size, int /*_BlockUse*/,
                                      char const * /*_FileName*/,
                                      int /*_LineNumber*/) {
  return ::operator new[](_Size);
}

inline void __CRTDECL
operator delete(void *_Block, int /*_BlockUse*/, char const * /*_FileName*/,
                int /*_LineNumber*/) __CLANG_NEW_DEBUG_NOEXCEPT {
  ::operator delete(_Block);
}

inline void __CRTDECL
operator delete[](void *_Block, int /*_BlockUse*/, char const * /*_FileName*/,
                  int /*_LineNumber*/) __CLANG_NEW_DEBUG_NOEXCEPT {
  ::operator delete[](_Block);
}

#undef __CLANG_NEW_DEBUG_NOEXCEPT

#endif /* __cplusplus */

#endif /* __CLANG_VCRUNTIME_NEW_DEBUG_H */
