// A stand-in for the UCRT's malloc.h, which defines _freea __inline and
// _mm_malloc and _mm_free as macros for the Microsoft aligned allocator.
#pragma once
#include <corecrt.h>
_CRT_BEGIN_C_HEADER
void __cdecl free(void *);
__inline void __CRTDECL _freea(void *_Memory) { free(_Memory); }
_CRT_END_C_HEADER
#define _mm_free(a) _aligned_free(a)
#define _mm_malloc(a, b) _aligned_malloc(a, b)
