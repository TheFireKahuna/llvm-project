// A stand-in for the UCRT's stdlib.h, which has declarations for C++ that
// collide with the C++ library's, and defines min and max in C.
#pragma once
#include <corecrt.h>
#define __need_NULL
#include <stddef.h>
_CRT_BEGIN_C_HEADER
int __cdecl __ucrt_stdlib_function(void) _CRT_NOEXCEPT;
_CRT_END_C_HEADER
#ifdef __cplusplus
#error "the UCRT's C++ declarations must stay hidden"
#else
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#if __STDC_VERSION__ == 201710L
#define __UCRT_STDLIB_SAW_C17 1
#endif
