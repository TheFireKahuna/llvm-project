// A stand-in for the UCRT's math.h, which has declarations for C++ that
// collide with the C++ library's, and defines some functions __inline.
#pragma once
#include <corecrt.h>
_CRT_BEGIN_C_HEADER
double __cdecl __ucrt_math_function(double) _CRT_NOEXCEPT;
__inline float __CRTDECL __ucrt_mathf(float _X) {
  return (float)__ucrt_math_function(_X);
}
_CRT_END_C_HEADER
#ifdef __cplusplus
#error "the UCRT's C++ declarations must stay hidden"
#endif
