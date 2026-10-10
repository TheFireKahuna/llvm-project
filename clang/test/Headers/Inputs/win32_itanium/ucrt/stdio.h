// A stand-in for the UCRT's stdio.h, which defines the formatted output
// functions _CRT_STDIO_INLINE, or only declares them if _NO_CRT_STDIO_INLINE
// is defined, and, as recent UCRTs do, declares its non-standard names as
// corecrt.h decided.
#pragma once
#include <corecrt.h>
#include <corecrt_stdio_config.h>
_CRT_BEGIN_C_HEADER
int __cdecl __stdio_common_vsprintf(char *, char const *, va_list);
_CRT_STDIO_INLINE int __CRTDECL sprintf(char *_Buffer, char const *_Format,
                                        ...)
#if defined(_NO_CRT_STDIO_INLINE)
    ;
#else
{
  va_list _ArgList;
  __crt_va_start(_ArgList, _Format);
  int _Result = __stdio_common_vsprintf(_Buffer, _Format, _ArgList);
  __crt_va_end(_ArgList);
  return _Result;
}
#endif
#if defined(_CRT_INTERNAL_NONSTDC_NAMES) && _CRT_INTERNAL_NONSTDC_NAMES
_CRT_NONSTDC_DEPRECATE(_fileno) int __cdecl fileno(void *);
#endif
_CRT_END_C_HEADER
