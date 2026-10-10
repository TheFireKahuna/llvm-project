// A stand-in for the UCRT's wchar.h, which defines the wmem functions __inline
// and includes corecrt_stdio_config.h through the wide stdio declarations, and
// corecrt_share.h and sys/stat.h.
#pragma once
#include <corecrt.h>
#include <corecrt_share.h>
#include <corecrt_stdio_config.h>
#include <sys/stat.h>
_CRT_BEGIN_C_HEADER
__inline wchar_t *__CRTDECL wmemset(wchar_t *_S, wchar_t _C, size_t _N) {
  size_t _I;
  for (_I = 0; _I < _N; ++_I)
    _S[_I] = _C;
  return _S;
}
_CRT_END_C_HEADER
