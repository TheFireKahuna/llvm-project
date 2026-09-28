/*===---- complex.h - UCRT complex.h wrapper -------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_COMPLEX_H
#define __CLANG_COMPLEX_H

#ifdef __cplusplus
#include_next <complex.h>
#else
/* The UCRT's complex.h declares its functions over the structures _Fcomplex,
 * _Dcomplex and _Lcomplex, which C99 _Complex values do not convert to. Each
 * structure is passed and returned exactly as the _Complex type of the same
 * element type, so declare the UCRT's exports with the standard types. Long
 * double has the representation of double on this target. */
#include <corecrt.h>

#define complex _Complex
#define _Complex_I (__extension__ 1.0iF)
#define I _Complex_I
#define CMPLX(x, y) __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
#define CMPLXL(x, y) __builtin_complex((long double)(x), (long double)(y))

#define __UCRT_COMPLEX_FUNCTIONS(T, S)                                         \
  _ACRTIMP T __cdecl cabs##S(T _Complex);                                      \
  _ACRTIMP T _Complex __cdecl cacos##S(T _Complex);                            \
  _ACRTIMP T _Complex __cdecl cacosh##S(T _Complex);                           \
  _ACRTIMP T __cdecl carg##S(T _Complex);                                      \
  _ACRTIMP T _Complex __cdecl casin##S(T _Complex);                            \
  _ACRTIMP T _Complex __cdecl casinh##S(T _Complex);                           \
  _ACRTIMP T _Complex __cdecl catan##S(T _Complex);                            \
  _ACRTIMP T _Complex __cdecl catanh##S(T _Complex);                           \
  _ACRTIMP T _Complex __cdecl ccos##S(T _Complex);                             \
  _ACRTIMP T _Complex __cdecl ccosh##S(T _Complex);                            \
  _ACRTIMP T _Complex __cdecl cexp##S(T _Complex);                             \
  _ACRTIMP T __cdecl cimag##S(T _Complex);                                     \
  _ACRTIMP T _Complex __cdecl clog##S(T _Complex);                             \
  _ACRTIMP T _Complex __cdecl conj##S(T _Complex);                             \
  _ACRTIMP T _Complex __cdecl cpow##S(T _Complex, T _Complex);                 \
  _ACRTIMP T _Complex __cdecl cproj##S(T _Complex);                            \
  _ACRTIMP T __cdecl creal##S(T _Complex);                                     \
  _ACRTIMP T _Complex __cdecl csin##S(T _Complex);                             \
  _ACRTIMP T _Complex __cdecl csinh##S(T _Complex);                            \
  _ACRTIMP T _Complex __cdecl csqrt##S(T _Complex);                            \
  _ACRTIMP T _Complex __cdecl ctan##S(T _Complex);                             \
  _ACRTIMP T _Complex __cdecl ctanh##S(T _Complex);

__UCRT_COMPLEX_FUNCTIONS(double, )
__UCRT_COMPLEX_FUNCTIONS(float, f)
__UCRT_COMPLEX_FUNCTIONS(long double, l)

#undef __UCRT_COMPLEX_FUNCTIONS
#endif

#endif /* __CLANG_COMPLEX_H */
