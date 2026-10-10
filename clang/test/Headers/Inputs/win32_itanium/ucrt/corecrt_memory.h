// A stand-in for the UCRT's corecrt_memory.h, which string.h and memory.h
// include, and which declares memccpy unless __STDC__ is true.
#pragma once
#include <corecrt.h>
_CRT_BEGIN_C_HEADER
#if !__STDC__
_CRT_NONSTDC_DEPRECATE(_memccpy)
void *__cdecl memccpy(void *, void const *, int, size_t);
#endif
_CRT_END_C_HEADER
