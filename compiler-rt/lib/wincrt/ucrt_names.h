//===-- ucrt_names.h - Names other import libraries offer too ---*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The functions of an image's C library that another import library a program
// may name offers too: the Windows SDK's private ntdllp.lib imports them from
// ntdll.dll, and Visual C++'s vcruntime.lib from vcruntime140.dll, in forms
// that differ. Every entry object wraps them (wincrt.h), and each __wrap_ name
// is an import of the function under its own name, so that the image binds
// the C library's whichever libraries it names.
//
// Only macros, since the module-definition file reads the list too.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_UCRT_NAMES_H
#define COMPILER_RT_LIB_WINCRT_UCRT_NAMES_H

// The Universal CRT's formatted I/O that ntdll.dll exports in reduced forms.
// wincrt defines these (formatted_io.c).
#define WINCRT_NTDLL_FORMATTED_IO(X)                                           \
  X(_snprintf)                                                                 \
  X(_snprintf_s)                                                               \
  X(_snscanf_s)                                                                \
  X(_snwprintf)                                                                \
  X(_snwprintf_s)                                                              \
  X(_snwscanf_s)                                                               \
  X(_swprintf)                                                                 \
  X(_vscprintf)                                                                \
  X(_vscwprintf)                                                               \
  X(_vsnprintf)                                                                \
  X(_vsnprintf_s)                                                              \
  X(_vsnwprintf)                                                               \
  X(_vsnwprintf_s)                                                             \
  X(_vswprintf)                                                                \
  X(sprintf)                                                                   \
  X(sprintf_s)                                                                 \
  X(sscanf)                                                                    \
  X(sscanf_s)                                                                  \
  X(swprintf)                                                                  \
  X(swprintf_s)                                                                \
  X(swscanf_s)                                                                 \
  X(vsprintf)                                                                  \
  X(vsprintf_s)                                                                \
  X(vswprintf_s)

// The functions that ucrtbase.dll exports, which clang_rt.ucrt_memory.lib
// imports under their __wrap_ names. ucrtbase.dll exports the function that
// _setjmp names only as setjmp, and the one that _setjmpex names, which is
// what setjmp calls on AArch64, only as __intrinsic_setjmpex.
#define WINCRT_UCRT_SHARED(X)                                                  \
  X(__C_specific_handler)                                                      \
  X(__isascii)                                                                 \
  X(__iscsym)                                                                  \
  X(__iscsymf)                                                                 \
  X(__toascii)                                                                 \
  X(_atoi64)                                                                   \
  X(_errno)                                                                    \
  X(_get_purecall_handler)                                                     \
  X(_i64toa)                                                                   \
  X(_i64toa_s)                                                                 \
  X(_i64tow)                                                                   \
  X(_i64tow_s)                                                                 \
  X(_itoa)                                                                     \
  X(_itoa_s)                                                                   \
  X(_itow)                                                                     \
  X(_itow_s)                                                                   \
  X(_lfind)                                                                    \
  X(_ltoa)                                                                     \
  X(_ltoa_s)                                                                   \
  X(_ltow)                                                                     \
  X(_ltow_s)                                                                   \
  X(_makepath_s)                                                               \
  X(_memccpy)                                                                  \
  X(_memicmp)                                                                  \
  X(_set_purecall_handler)                                                     \
  X(_splitpath)                                                                \
  X(_splitpath_s)                                                              \
  X(_stricmp)                                                                  \
  X(_strlwr)                                                                   \
  X(_strlwr_s)                                                                 \
  X(_strnicmp)                                                                 \
  X(_strnset_s)                                                                \
  X(_strset_s)                                                                 \
  X(_strupr)                                                                   \
  X(_strupr_s)                                                                 \
  X(_ui64toa)                                                                  \
  X(_ui64toa_s)                                                                \
  X(_ui64tow)                                                                  \
  X(_ui64tow_s)                                                                \
  X(_ultoa)                                                                    \
  X(_ultoa_s)                                                                  \
  X(_ultow)                                                                    \
  X(_ultow_s)                                                                  \
  X(_wcsicmp)                                                                  \
  X(_wcslwr)                                                                   \
  X(_wcslwr_s)                                                                 \
  X(_wcsnicmp)                                                                 \
  X(_wcsnset_s)                                                                \
  X(_wcsset_s)                                                                 \
  X(_wcstoi64)                                                                 \
  X(_wcstoui64)                                                                \
  X(_wcsupr)                                                                   \
  X(_wcsupr_s)                                                                 \
  X(_wmakepath_s)                                                              \
  X(_wsplitpath_s)                                                             \
  X(_wtoi)                                                                     \
  X(_wtoi64)                                                                   \
  X(_wtol)                                                                     \
  X(abs)                                                                       \
  X(atan)                                                                      \
  X(atan2)                                                                     \
  X(atoi)                                                                      \
  X(atol)                                                                      \
  X(bsearch)                                                                   \
  X(bsearch_s)                                                                 \
  X(ceil)                                                                      \
  X(cos)                                                                       \
  X(fabs)                                                                      \
  X(floor)                                                                     \
  X(isalnum)                                                                   \
  X(isalpha)                                                                   \
  X(iscntrl)                                                                   \
  X(isdigit)                                                                   \
  X(isgraph)                                                                   \
  X(islower)                                                                   \
  X(isprint)                                                                   \
  X(ispunct)                                                                   \
  X(isspace)                                                                   \
  X(isupper)                                                                   \
  X(iswalnum)                                                                  \
  X(iswalpha)                                                                  \
  X(iswascii)                                                                  \
  X(iswctype)                                                                  \
  X(iswdigit)                                                                  \
  X(iswgraph)                                                                  \
  X(iswlower)                                                                  \
  X(iswprint)                                                                  \
  X(iswspace)                                                                  \
  X(iswxdigit)                                                                 \
  X(isxdigit)                                                                  \
  X(labs)                                                                      \
  X(log)                                                                       \
  X(longjmp)                                                                   \
  X(mbstowcs)                                                                  \
  X(memchr)                                                                    \
  X(memcmp)                                                                    \
  X(memcpy)                                                                    \
  X(memcpy_s)                                                                  \
  X(memmove)                                                                   \
  X(memmove_s)                                                                 \
  X(memset)                                                                    \
  X(pow)                                                                       \
  X(qsort)                                                                     \
  X(qsort_s)                                                                   \
  X(sin)                                                                       \
  X(sqrt)                                                                      \
  X(strcat)                                                                    \
  X(strcat_s)                                                                  \
  X(strchr)                                                                    \
  X(strcmp)                                                                    \
  X(strcpy)                                                                    \
  X(strcpy_s)                                                                  \
  X(strcspn)                                                                   \
  X(strlen)                                                                    \
  X(strncat)                                                                   \
  X(strncat_s)                                                                 \
  X(strncmp)                                                                   \
  X(strncpy)                                                                   \
  X(strncpy_s)                                                                 \
  X(strnlen)                                                                   \
  X(strpbrk)                                                                   \
  X(strrchr)                                                                   \
  X(strspn)                                                                    \
  X(strstr)                                                                    \
  X(strtok_s)                                                                  \
  X(strtol)                                                                    \
  X(strtoul)                                                                   \
  X(tan)                                                                       \
  X(tolower)                                                                   \
  X(toupper)                                                                   \
  X(towlower)                                                                  \
  X(towupper)                                                                  \
  X(wcscat)                                                                    \
  X(wcscat_s)                                                                  \
  X(wcschr)                                                                    \
  X(wcscmp)                                                                    \
  X(wcscpy)                                                                    \
  X(wcscpy_s)                                                                  \
  X(wcscspn)                                                                   \
  X(wcslen)                                                                    \
  X(wcsncat)                                                                   \
  X(wcsncat_s)                                                                 \
  X(wcsncmp)                                                                   \
  X(wcsncpy)                                                                   \
  X(wcsncpy_s)                                                                 \
  X(wcsnlen)                                                                   \
  X(wcspbrk)                                                                   \
  X(wcsrchr)                                                                   \
  X(wcsspn)                                                                    \
  X(wcsstr)                                                                    \
  X(wcstok_s)                                                                  \
  X(wcstol)                                                                    \
  X(wcstombs)                                                                  \
  X(wcstoul)                                                                   \
  X(_setjmp, setjmp)                                                           \
  X(_setjmpex, __intrinsic_setjmpex)

#endif // COMPILER_RT_LIB_WINCRT_UCRT_NAMES_H
