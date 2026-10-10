// A stand-in for the UCRT's tchar.h, which defines the generic-text functions
// __inline unless __STDC__ is defined.
#pragma once
#include <corecrt.h>
#if !__STDC__
_CRT_BEGIN_C_HEADER
__inline size_t __CRTDECL _tclen(const char *_Cpc) { return *_Cpc != 0; }
_CRT_END_C_HEADER
#endif
