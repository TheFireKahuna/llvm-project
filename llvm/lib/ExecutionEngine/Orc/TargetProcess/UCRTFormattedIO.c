/*===-- UCRTFormattedIO.c - The Universal CRT's formatted I/O ---------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===----------------------------------------------------------------------===
 *
 * The Universal CRT defines printf, scanf and the rest of its formatted I/O
 * inline in its headers, over functions that ucrtbase.dll exports, and no DLL
 * exports them. Code compiled at run time that calls them binds to this
 * program's definitions, compiled here from those headers, which use the
 * program's streams. The file is C, where none of the names is overloaded.
 *
 *===----------------------------------------------------------------------===
 */

/* A static C runtime is private to the program, and code compiled at run time
 * is given ucrtbase.dll's instead. The definitions here must then use that
 * one too: they are private to this file, since the program's own are built
 * on its C runtime, and they call the Universal CRT through import pointers,
 * which bindUCRT sets. */
#if defined(_MSC_VER) && !defined(_DLL)
#define _CRT_STDIO_INLINE static __inline
#define _ACRTIMP __declspec(dllimport)
#define LLVM_ORC_BIND_UCRT
#endif

#include <stdio.h>
#include <string.h>

/* The Universal CRT's stdio.h defines _CRT_STDIO_INLINE. */
#ifdef _CRT_STDIO_INLINE
#include <conio.h>
#include <wchar.h>
#endif

typedef void (*FormattedIOFunction)(void);

#ifdef LLVM_ORC_BIND_UCRT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Every function of the Universal CRT that the inline definitions call. */
/* clang-format off */
#define UCRT_IMPORTS(IMPORT)                                                   \
  IMPORT(__acrt_iob_func)                                                      \
  IMPORT(__conio_common_vcprintf)      IMPORT(__conio_common_vcwprintf)        \
  IMPORT(__conio_common_vcprintf_p)    IMPORT(__conio_common_vcwprintf_p)      \
  IMPORT(__conio_common_vcprintf_s)    IMPORT(__conio_common_vcwprintf_s)      \
  IMPORT(__conio_common_vcscanf)       IMPORT(__conio_common_vcwscanf)         \
  IMPORT(__stdio_common_vfprintf)      IMPORT(__stdio_common_vfwprintf)        \
  IMPORT(__stdio_common_vfprintf_p)    IMPORT(__stdio_common_vfwprintf_p)      \
  IMPORT(__stdio_common_vfprintf_s)    IMPORT(__stdio_common_vfwprintf_s)      \
  IMPORT(__stdio_common_vfscanf)       IMPORT(__stdio_common_vfwscanf)         \
  IMPORT(__stdio_common_vsnprintf_s)   IMPORT(__stdio_common_vsnwprintf_s)     \
  IMPORT(__stdio_common_vsprintf)      IMPORT(__stdio_common_vswprintf)        \
  IMPORT(__stdio_common_vsprintf_p)    IMPORT(__stdio_common_vswprintf_p)      \
  IMPORT(__stdio_common_vsprintf_s)    IMPORT(__stdio_common_vswprintf_s)      \
  IMPORT(__stdio_common_vsscanf)       IMPORT(__stdio_common_vswscanf)
/* clang-format on */

/* A function's import pointer; x86 prefixes both names with an underscore. */
#ifdef _M_IX86
#define IMPORT_POINTER(F) _imp__##F
#else
#define IMPORT_POINTER(F) __imp_##F
#endif

#define IMPORT(F) FARPROC IMPORT_POINTER(F);
UCRT_IMPORTS(IMPORT)
#undef IMPORT

static BOOL CALLBACK bindUCRT(PINIT_ONCE Once, PVOID Parameter,
                              PVOID *Context) {
  HMODULE UCRT =
      LoadLibraryExW(L"ucrtbase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
#define IMPORT(F)                                                              \
  if (!(IMPORT_POINTER(F) = GetProcAddress(UCRT, #F)))                         \
    return FALSE;
  UCRT_IMPORTS(IMPORT)
#undef IMPORT
  return TRUE;
}
#endif

FormattedIOFunction llvm_orc_find_ucrt_formatted_io(const char *Name) {
#ifdef LLVM_ORC_BIND_UCRT
  static INIT_ONCE Bound = INIT_ONCE_STATIC_INIT;
  if (!InitOnceExecuteOnce(&Bound, bindUCRT, NULL, NULL))
    return NULL;
#endif
#ifdef _CRT_STDIO_INLINE
#define FUNCTION(F) {#F, (FormattedIOFunction)F},
  /* clang-format off */
  static const struct {
    const char *Name;
    FormattedIOFunction Address;
  } Functions[] = {
      FUNCTION(__swprintf_l)
      FUNCTION(__vswprintf_l)
      FUNCTION(_cprintf)
      FUNCTION(_cprintf_l)
      FUNCTION(_cprintf_p)
      FUNCTION(_cprintf_p_l)
      FUNCTION(_cprintf_s)
      FUNCTION(_cprintf_s_l)
      FUNCTION(_cscanf)
      FUNCTION(_cscanf_l)
      FUNCTION(_cscanf_s)
      FUNCTION(_cscanf_s_l)
      FUNCTION(_cwprintf)
      FUNCTION(_cwprintf_l)
      FUNCTION(_cwprintf_p)
      FUNCTION(_cwprintf_p_l)
      FUNCTION(_cwprintf_s)
      FUNCTION(_cwprintf_s_l)
      FUNCTION(_cwscanf)
      FUNCTION(_cwscanf_l)
      FUNCTION(_cwscanf_s)
      FUNCTION(_cwscanf_s_l)
      FUNCTION(_fprintf_l)
      FUNCTION(_fprintf_p)
      FUNCTION(_fprintf_p_l)
      FUNCTION(_fprintf_s_l)
      FUNCTION(_fscanf_l)
      FUNCTION(_fscanf_s_l)
      FUNCTION(_fwprintf_l)
      FUNCTION(_fwprintf_p)
      FUNCTION(_fwprintf_p_l)
      FUNCTION(_fwprintf_s_l)
      FUNCTION(_fwscanf_l)
      FUNCTION(_fwscanf_s_l)
      FUNCTION(_printf_l)
      FUNCTION(_printf_p)
      FUNCTION(_printf_p_l)
      FUNCTION(_printf_s_l)
      FUNCTION(_scanf_l)
      FUNCTION(_scanf_s_l)
      FUNCTION(_scprintf)
      FUNCTION(_scprintf_l)
      FUNCTION(_scprintf_p)
      FUNCTION(_scprintf_p_l)
      FUNCTION(_scwprintf)
      FUNCTION(_scwprintf_l)
      FUNCTION(_scwprintf_p)
      FUNCTION(_scwprintf_p_l)
      FUNCTION(_snprintf)
      FUNCTION(_snprintf_c)
      FUNCTION(_snprintf_c_l)
      FUNCTION(_snprintf_l)
      FUNCTION(_snprintf_s)
      FUNCTION(_snprintf_s_l)
      FUNCTION(_snscanf)
      FUNCTION(_snscanf_l)
      FUNCTION(_snscanf_s)
      FUNCTION(_snscanf_s_l)
      FUNCTION(_snwprintf)
      FUNCTION(_snwprintf_l)
      FUNCTION(_snwprintf_s)
      FUNCTION(_snwprintf_s_l)
      FUNCTION(_snwscanf)
      FUNCTION(_snwscanf_l)
      FUNCTION(_snwscanf_s)
      FUNCTION(_snwscanf_s_l)
      FUNCTION(_sprintf_l)
      FUNCTION(_sprintf_p)
      FUNCTION(_sprintf_p_l)
      FUNCTION(_sprintf_s_l)
      FUNCTION(_sscanf_l)
      FUNCTION(_sscanf_s_l)
      FUNCTION(_swprintf)
      FUNCTION(_swprintf_c)
      FUNCTION(_swprintf_c_l)
      FUNCTION(_swprintf_l)
      FUNCTION(_swprintf_p)
      FUNCTION(_swprintf_p_l)
      FUNCTION(_swprintf_s_l)
      FUNCTION(_swscanf_l)
      FUNCTION(_swscanf_s_l)
      FUNCTION(_vcprintf)
      FUNCTION(_vcprintf_l)
      FUNCTION(_vcprintf_p)
      FUNCTION(_vcprintf_p_l)
      FUNCTION(_vcprintf_s)
      FUNCTION(_vcprintf_s_l)
      FUNCTION(_vcscanf)
      FUNCTION(_vcscanf_l)
      FUNCTION(_vcscanf_s)
      FUNCTION(_vcscanf_s_l)
      FUNCTION(_vcwprintf)
      FUNCTION(_vcwprintf_l)
      FUNCTION(_vcwprintf_p)
      FUNCTION(_vcwprintf_p_l)
      FUNCTION(_vcwprintf_s)
      FUNCTION(_vcwprintf_s_l)
      FUNCTION(_vcwscanf)
      FUNCTION(_vcwscanf_l)
      FUNCTION(_vcwscanf_s)
      FUNCTION(_vcwscanf_s_l)
      FUNCTION(_vfprintf_l)
      FUNCTION(_vfprintf_p)
      FUNCTION(_vfprintf_p_l)
      FUNCTION(_vfprintf_s_l)
      FUNCTION(_vfscanf_l)
      FUNCTION(_vfscanf_s_l)
      FUNCTION(_vfwprintf_l)
      FUNCTION(_vfwprintf_p)
      FUNCTION(_vfwprintf_p_l)
      FUNCTION(_vfwprintf_s_l)
      FUNCTION(_vfwscanf_l)
      FUNCTION(_vfwscanf_s_l)
      FUNCTION(_vprintf_l)
      FUNCTION(_vprintf_p)
      FUNCTION(_vprintf_p_l)
      FUNCTION(_vprintf_s_l)
      FUNCTION(_vscanf_l)
      FUNCTION(_vscanf_s_l)
      FUNCTION(_vscprintf)
      FUNCTION(_vscprintf_l)
      FUNCTION(_vscprintf_p)
      FUNCTION(_vscprintf_p_l)
      FUNCTION(_vscwprintf)
      FUNCTION(_vscwprintf_l)
      FUNCTION(_vscwprintf_p)
      FUNCTION(_vscwprintf_p_l)
      FUNCTION(_vsnprintf)
      FUNCTION(_vsnprintf_c)
      FUNCTION(_vsnprintf_c_l)
      FUNCTION(_vsnprintf_l)
      FUNCTION(_vsnprintf_s)
      FUNCTION(_vsnprintf_s_l)
      FUNCTION(_vsnwprintf)
      FUNCTION(_vsnwprintf_l)
      FUNCTION(_vsnwprintf_s)
      FUNCTION(_vsnwprintf_s_l)
      FUNCTION(_vsnwscanf_l)
      FUNCTION(_vsnwscanf_s_l)
      FUNCTION(_vsprintf_l)
      FUNCTION(_vsprintf_p)
      FUNCTION(_vsprintf_p_l)
      FUNCTION(_vsprintf_s_l)
      FUNCTION(_vsscanf_l)
      FUNCTION(_vsscanf_s_l)
      FUNCTION(_vswprintf)
      FUNCTION(_vswprintf_c)
      FUNCTION(_vswprintf_c_l)
      FUNCTION(_vswprintf_l)
      FUNCTION(_vswprintf_p)
      FUNCTION(_vswprintf_p_l)
      FUNCTION(_vswprintf_s_l)
      FUNCTION(_vswscanf_l)
      FUNCTION(_vswscanf_s_l)
      FUNCTION(_vwprintf_l)
      FUNCTION(_vwprintf_p)
      FUNCTION(_vwprintf_p_l)
      FUNCTION(_vwprintf_s_l)
      FUNCTION(_vwscanf_l)
      FUNCTION(_vwscanf_s_l)
      FUNCTION(_wprintf_l)
      FUNCTION(_wprintf_p)
      FUNCTION(_wprintf_p_l)
      FUNCTION(_wprintf_s_l)
      FUNCTION(_wscanf_l)
      FUNCTION(_wscanf_s_l)
      FUNCTION(cprintf)
      FUNCTION(cscanf)
      FUNCTION(fprintf)
      FUNCTION(fprintf_s)
      FUNCTION(fscanf)
      FUNCTION(fscanf_s)
      FUNCTION(fwprintf)
      FUNCTION(fwprintf_s)
      FUNCTION(fwscanf)
      FUNCTION(fwscanf_s)
      FUNCTION(printf)
      FUNCTION(printf_s)
      FUNCTION(scanf)
      FUNCTION(scanf_s)
      FUNCTION(snprintf)
      FUNCTION(sprintf)
      FUNCTION(sprintf_s)
      FUNCTION(sscanf)
      FUNCTION(sscanf_s)
      FUNCTION(swprintf)
      FUNCTION(swprintf_s)
      FUNCTION(swscanf)
      FUNCTION(swscanf_s)
      FUNCTION(vfprintf)
      FUNCTION(vfprintf_s)
      FUNCTION(vfscanf)
      FUNCTION(vfscanf_s)
      FUNCTION(vfwprintf)
      FUNCTION(vfwprintf_s)
      FUNCTION(vfwscanf)
      FUNCTION(vfwscanf_s)
      FUNCTION(vprintf)
      FUNCTION(vprintf_s)
      FUNCTION(vscanf)
      FUNCTION(vscanf_s)
      FUNCTION(vsnprintf)
      FUNCTION(vsnprintf_s)
      FUNCTION(vsprintf)
      FUNCTION(vsprintf_s)
      FUNCTION(vsscanf)
      FUNCTION(vsscanf_s)
      FUNCTION(vswprintf)
      FUNCTION(vswprintf_s)
      FUNCTION(vswscanf)
      FUNCTION(vswscanf_s)
      FUNCTION(vwprintf)
      FUNCTION(vwprintf_s)
      FUNCTION(vwscanf)
      FUNCTION(vwscanf_s)
      FUNCTION(wprintf)
      FUNCTION(wprintf_s)
      FUNCTION(wscanf)
      FUNCTION(wscanf_s)
  };
  /* clang-format on */
#undef FUNCTION
  for (size_t I = 0; I != sizeof(Functions) / sizeof(Functions[0]); ++I)
    if (!strcmp(Functions[I].Name, Name))
      return Functions[I].Address;
#endif
  return NULL;
}
