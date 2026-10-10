// A stand-in for the Windows SDK's winnt.h, which chooses its declaration
// macros by _MSC_VER unless they are defined already, and defines a few
// others by _MSC_VER unconditionally.
#pragma once
#ifndef DECLSPEC_SELECTANY
#if _MSC_VER >= 1100
#define DECLSPEC_SELECTANY __declspec(selectany)
#else
#define DECLSPEC_SELECTANY
#endif
#endif
#ifndef FORCEINLINE
#if _MSC_VER >= 1200
#define FORCEINLINE __forceinline
#else
#define FORCEINLINE __inline
#endif
#endif
#ifndef DECLSPEC_UUID
#if _MSC_VER >= 1100 && defined(__cplusplus)
#define DECLSPEC_UUID(x) __declspec(uuid(x))
#else
#define DECLSPEC_UUID(x)
#endif
#endif
#ifndef DECLSPEC_GUARDNOCF
#if _MSC_FULL_VER >= 170065501
#define DECLSPEC_GUARDNOCF __declspec(guard(nocf))
#else
#define DECLSPEC_GUARDNOCF
#endif
#endif
#ifndef DECLSPEC_GUARD_SUPPRESS
#if _MSC_FULL_VER >= 181040116
#define DECLSPEC_GUARD_SUPPRESS __declspec(guard(suppress))
#else
#define DECLSPEC_GUARD_SUPPRESS
#endif
#endif
#ifdef __cplusplus
#if _MSC_VER >= 1900
#define WIN_NOEXCEPT noexcept
#else
#define WIN_NOEXCEPT throw()
#endif
#if _MSC_VER >= 1300
#define TYPE_ALIGNMENT(t) __alignof(t)
#endif
#if _MSC_VER >= 1900
#define _ENUM_FLAG_CONSTEXPR constexpr
#else
#define _ENUM_FLAG_CONSTEXPR
#endif
#else
#define WIN_NOEXCEPT
#endif
#if _MSC_VER > 1200
#define DEFAULT_UNREACHABLE default: __assume(0)
#else
#define DEFAULT_UNREACHABLE
#endif
FORCEINLINE int WinntTwice(int x) { return 2 * x; }
