// A stand-in for the UCRT's string.h, which declares its non-standard names
// unless __STDC__ is true, some of them in corecrt_memory.h.
#pragma once
#include <corecrt.h>
#include <corecrt_memory.h>
_CRT_BEGIN_C_HEADER
#if !__STDC__
_CRT_NONSTDC_DEPRECATE(_strdup) char *__cdecl strdup(char const *);
#endif
_CRT_END_C_HEADER
