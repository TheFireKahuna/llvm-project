// A stand-in for the UCRT's fenv.h, which defines feraiseexcept __inline.
#pragma once
#include <corecrt.h>
_CRT_BEGIN_C_HEADER
__inline int __CRTDECL feraiseexcept(int _Except) { return _Except; }
_CRT_END_C_HEADER
