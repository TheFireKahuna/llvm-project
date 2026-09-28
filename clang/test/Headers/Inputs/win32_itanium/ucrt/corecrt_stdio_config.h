// A stand-in for the UCRT's corecrt_stdio_config.h, which defines the stdio
// option functions __inline.
#pragma once
#include <corecrt.h>
#ifndef _CRT_STDIO_INLINE
#define _CRT_STDIO_INLINE __inline
#endif
#if defined(_CRT_STDIO_ISO_WIDE_SPECIFIERS) &&                                 \
    defined(_CRT_STDIO_LEGACY_WIDE_SPECIFIERS)
#error "_CRT_STDIO_ISO_WIDE_SPECIFIERS and _CRT_STDIO_LEGACY_WIDE_SPECIFIERS"
#endif
_CRT_BEGIN_C_HEADER
__inline unsigned long long *__CRTDECL __local_stdio_printf_options(void) {
  static unsigned long long _OptionsStorage;
  return &_OptionsStorage;
}
_CRT_END_C_HEADER
