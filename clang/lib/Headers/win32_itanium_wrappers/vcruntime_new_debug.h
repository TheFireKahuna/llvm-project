/*===---- vcruntime_new_debug.h - debug operator new from the VCRuntime -----===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_vcruntime_new_debug_h
#define __clang_vcruntime_new_debug_h


/*
 * Zero-Visual-Studio-headers targets (Windows Itanium): the VCRuntime is not
 * on the include path, but UCRT's <crtdbg.h> includes this header
 * unconditionally. The debug CRT (ucrtbased/vcruntimed) is never linked and
 * libc++ owns operator new/delete. Mirror the real header's transitive
 * <vcruntime_new.h> include (itself a no-op wrapper under libc++), and
 * provide the debug block-type overloads as inline forwards to the plain
 * global forms so _CRTDBG_MAP_ALLOC-style code compiles and behaves exactly
 * like a release-CRT build.
 */
#include <vcruntime_new.h>

#ifdef __cplusplus

#if __cplusplus >= 201103L
#define __CLANG_NEW_DEBUG_NOEXCEPT noexcept
#else
#define __CLANG_NEW_DEBUG_NOEXCEPT throw()
#endif

/* The plain forms may not be declared yet (libc++'s <new> owns them but need
 * not have been included); declaring the replaceable signatures is always
 * valid. __SIZE_TYPE__ is the same type as std::size_t. */
void *operator new(__SIZE_TYPE__ _Size);
void *operator new[](__SIZE_TYPE__ _Size);
void operator delete(void *_Block) __CLANG_NEW_DEBUG_NOEXCEPT;
void operator delete[](void *_Block) __CLANG_NEW_DEBUG_NOEXCEPT;

inline void *operator new(__SIZE_TYPE__ _Size, int /*_BlockUse*/,
                          char const * /*_FileName*/, int /*_LineNumber*/) {
  return ::operator new(_Size);
}

inline void *operator new[](__SIZE_TYPE__ _Size, int /*_BlockUse*/,
                            char const * /*_FileName*/, int /*_LineNumber*/) {
  return ::operator new[](_Size);
}

inline void operator delete(void *_Block, int /*_BlockUse*/,
                            char const * /*_FileName*/,
                            int /*_LineNumber*/) __CLANG_NEW_DEBUG_NOEXCEPT {
  ::operator delete(_Block);
}

inline void operator delete[](void *_Block, int /*_BlockUse*/,
                              char const * /*_FileName*/,
                              int /*_LineNumber*/) __CLANG_NEW_DEBUG_NOEXCEPT {
  ::operator delete[](_Block);
}

#undef __CLANG_NEW_DEBUG_NOEXCEPT

#endif /* __cplusplus */


#endif /* __clang_vcruntime_new_debug_h */
