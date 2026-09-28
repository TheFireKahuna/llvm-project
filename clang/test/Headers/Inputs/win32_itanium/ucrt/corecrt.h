// A stand-in for the UCRT's corecrt.h: it includes vcruntime.h and uses
// what that header provides.
#pragma once
#include <vcruntime.h>

#ifdef __cplusplus
#define _CONST_RETURN const
#else
#define _CONST_RETURN
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

// The UCRT's headers settle their configuration on whether the first
// inclusion of corecrt.h sees _MSC_EXTENSIONS.
#ifdef _MSC_EXTENSIONS
#define __UCRT_CORECRT_SAW_MSC_EXTENSIONS 1
#endif
