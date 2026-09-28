// A stand-in for the UCRT's io.h, which, as recent UCRTs do, declares its POSIX
// names as corecrt.h decided.
#pragma once
#include <corecrt_share.h>
_CRT_BEGIN_C_HEADER
#if defined(_CRT_INTERNAL_NONSTDC_NAMES) && _CRT_INTERNAL_NONSTDC_NAMES
_CRT_NONSTDC_DEPRECATE(_open) int __cdecl open(char const *, int, ...);
#endif
_CRT_END_C_HEADER
