// A stand-in for the UCRT's stdlib.h, which has declarations for C++ that
// collide with the C++ library's, and declares its non-standard names, among
// them the min and max macros in C, unless __STDC__ is true.
#pragma once
#include <corecrt.h>
#define __need_NULL
#include <stddef.h>
_CRT_BEGIN_C_HEADER
int __cdecl __ucrt_stdlib_function(void) _CRT_NOEXCEPT;
void *__cdecl malloc(size_t);
void __cdecl free(void *);
_CRT_END_C_HEADER
#ifdef __cplusplus
#error "the UCRT's C++ declarations must stay hidden"
#endif
#if !__STDC__
_CRT_BEGIN_C_HEADER
_CRT_NONSTDC_DEPRECATE(_itoa) char *__cdecl itoa(int, char *, int);
_CRT_END_C_HEADER
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define environ _environ
#endif
#if __STDC_VERSION__ == 201710L
#define __UCRT_STDLIB_SAW_C17 1
#endif
