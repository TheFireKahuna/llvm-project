// A stand-in for the UCRT's math.h, which has declarations for C++ that
// collide with the C++ library's, defines some functions __inline, and
// declares its non-standard names unless __STDC__ is true.
#pragma once
#include <corecrt.h>
_CRT_BEGIN_C_HEADER
double __cdecl __ucrt_math_function(double) _CRT_NOEXCEPT;
__inline float __CRTDECL __ucrt_mathf(float _X) {
  return (float)__ucrt_math_function(_X);
}
#if !__STDC__
_CRT_NONSTDC_DEPRECATE(_j0) double __cdecl j0(double);
#endif
_CRT_END_C_HEADER
#if !__STDC__
#ifndef __cplusplus
#define complex _complex
#endif
#define DOMAIN _DOMAIN
#endif
#ifdef __cplusplus
#error "the UCRT's C++ declarations must stay hidden"
#endif
