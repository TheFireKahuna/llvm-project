// A stand-in for the UCRT's ctype.h, which defines the helpers of the _is*_l
// macros __inline and the __ascii_* helpers __forceinline.
#pragma once
#include <corecrt.h>
_CRT_BEGIN_C_HEADER
__inline int __CRTDECL _chvalidchk_l(int _C, int _Mask) { return _C & _Mask; }
__forceinline int __CRTDECL __ascii_tolower(int const _C) { return _C | 32; }
_CRT_END_C_HEADER
