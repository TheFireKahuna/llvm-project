// A stand-in for the UCRT's corecrt.h: it includes vcruntime.h and uses
// what that header provides.
#pragma once
#include <vcruntime.h>

_CRT_BEGIN_C_HEADER
struct __ucrt_packed {
  char c;
  __int128 x;
};
_VCRTIMP size_t __CRTDECL __ucrt_function(wchar_t const *, va_list)
    _CRT_INSECURE_DEPRECATE(__ucrt_function_s);
_CRT_DEPRECATE_TEXT("deprecated") void __ucrt_deprecated(void);
_CRT_END_C_HEADER
