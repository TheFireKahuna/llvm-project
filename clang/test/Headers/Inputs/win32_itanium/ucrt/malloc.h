// A stand-in for the UCRT's malloc.h, which defines _freea __inline.
#pragma once
#include <corecrt.h>
_CRT_BEGIN_C_HEADER
void __cdecl free(void *);
__inline void __CRTDECL _freea(void *_Memory) { free(_Memory); }
_CRT_END_C_HEADER
