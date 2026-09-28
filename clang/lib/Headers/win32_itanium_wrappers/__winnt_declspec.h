/*===---- __winnt_declspec.h - Windows SDK declaration macros --------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* ntdef.h and winnt.h define these macros only when they do not exist yet,
 * choosing the attribute when _MSC_VER names a compiler that has it and
 * nothing, or an unsuitable fallback, otherwise. Without _MSC_VER, define
 * them for what clang supports. The attributes clang does not support are left
 * to the SDK's empty fallbacks: DECLSPEC_NOVTABLE, DECLSPEC_GUARDIGNORE and
 * DECLSPEC_GUARD_SUPPRESS. */

#ifndef __CLANG_WINNT_DECLSPEC_H
#define __CLANG_WINNT_DECLSPEC_H

#ifndef DECLSPEC_NORETURN
#define DECLSPEC_NORETURN __declspec(noreturn)
#endif

#ifndef DECLSPEC_NOTHROW
#define DECLSPEC_NOTHROW __declspec(nothrow)
#endif

#ifndef DECLSPEC_RESTRICT
#define DECLSPEC_RESTRICT __declspec(restrict)
#endif

/* Without it the SDK's structures that need extended alignment, such as
 * CONTEXT, lose it. */
#ifndef DECLSPEC_ALIGN
#define DECLSPEC_ALIGN(x) __declspec(align(x))
#endif

#if defined(__cplusplus) && !defined(DECLSPEC_UUID)
#define DECLSPEC_UUID(x) __declspec(uuid(x))
#endif

/* Without it each unit that defines a GUID with DEFINE_GUID defines a strong
 * symbol. */
#ifndef DECLSPEC_SELECTANY
#define DECLSPEC_SELECTANY __declspec(selectany)
#endif

#ifndef DECLSPEC_SAFEBUFFERS
#define DECLSPEC_SAFEBUFFERS __declspec(safebuffers)
#endif

#ifndef DECLSPEC_NOINLINE
#define DECLSPEC_NOINLINE __declspec(noinline)
#endif

#ifndef DECLSPEC_GUARDNOCF
#define DECLSPEC_GUARDNOCF __declspec(guard(nocf))
#endif

/* The fallback, __inline, has C99 inline semantics in C, so a call that is
 * not inlined would refer to a function that no library defines. */
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif

#ifndef DECLSPEC_DEPRECATED
#define DECLSPEC_DEPRECATED __declspec(deprecated)
#define DEPRECATE_SUPPORTED
#endif

/* The C fallback is not valid C++. */
#if defined(__cplusplus) && !defined(TYPE_ALIGNMENT)
#define TYPE_ALIGNMENT(t) __alignof(t)
#endif

#endif /* __CLANG_WINNT_DECLSPEC_H */
