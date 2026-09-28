// A stand-in for the UCRT's corecrt.h: it includes vcruntime.h and uses
// what that header provides.
#pragma once
#include <vcruntime.h>

#ifdef __cplusplus
#define _CONST_RETURN const
#else
#define _CONST_RETURN
#endif

#ifndef _ACRTIMP
#ifdef _DLL
#define _ACRTIMP __declspec(dllimport)
#else
#define _ACRTIMP
#endif
#endif

#ifndef _CRT_NOEXCEPT
#ifdef __cplusplus
#define _CRT_NOEXCEPT noexcept
#else
#define _CRT_NOEXCEPT
#endif
#endif

_CRT_BEGIN_C_HEADER
struct __ucrt_packed {
  char c;
  __int128 x;
};
_VCRTIMP size_t __CRTDECL __ucrt_function(wchar_t const *, va_list)
    _CRT_INSECURE_DEPRECATE(__ucrt_function_s);
_CRT_DEPRECATE_TEXT("deprecated") void __ucrt_deprecated(void);
_CRT_END_C_HEADER

#if defined _CRT_NONSTDC_NO_DEPRECATE && !defined _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_DEPRECATE
#ifdef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_DEPRECATE(_NewName)
#else
#define _CRT_NONSTDC_DEPRECATE(_NewName)                                       \
  _CRT_DEPRECATE_TEXT("The POSIX name for this item is deprecated.")
#endif
#endif

// Recent UCRTs decide once whether to declare the non-standard names.
#if (defined _CRT_DECLARE_NONSTDC_NAMES && _CRT_DECLARE_NONSTDC_NAMES) ||     \
    (!defined _CRT_DECLARE_NONSTDC_NAMES && !__STDC__)
#define _CRT_INTERNAL_NONSTDC_NAMES 1
#else
#define _CRT_INTERNAL_NONSTDC_NAMES 0
#endif

// The UCRT's headers settle their configuration on whether the first
// inclusion of corecrt.h sees _MSC_EXTENSIONS.
#ifdef _MSC_EXTENSIONS
#define __UCRT_CORECRT_SAW_MSC_EXTENSIONS 1
#endif
