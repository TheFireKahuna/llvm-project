//===-- shared_imports.h - Imports other import libraries offer ---*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The functions an image imports from the Universal CRT that another import
// library a program may name also offers: the Windows SDK's private
// ntdllp.lib imports them from ntdll.dll, and vcruntime.lib from
// vcruntime140.dll, whose forms differ from the Universal CRT's. Every entry
// object wraps them, and clang_rt.ucrt_memory.lib imports each __wrap_ name
// as the function itself, so that the image binds the Universal CRT's
// function whichever libraries it names, under its own name.
//
// Each entry names the function and the module that ucrt.lib, or for the
// functions it lacks clang_rt.ucrt_memory.lib, imports it from: ucrtbase for
// ucrtbase.dll, and otherwise the API set api-ms-win-crt-<module>-l1-1-0.dll.
// The build reads the entries from this file to generate the imports.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_SHARED_IMPORTS_H
#define COMPILER_RT_LIB_WINCRT_SHARED_IMPORTS_H

#define WINCRT_SHARED_IMPORTS(X)                                               \
  X(__C_specific_handler, ucrtbase)                                            \
  X(_get_purecall_handler, ucrtbase)                                           \
  X(_set_purecall_handler, ucrtbase)                                           \
  X(longjmp, ucrtbase)                                                         \
  X(memchr, ucrtbase)                                                          \
  X(memcmp, ucrtbase)                                                          \
  X(memcpy, ucrtbase)                                                          \
  X(memmove, ucrtbase)                                                         \
  X(memset, ucrtbase)                                                          \
  X(strchr, ucrtbase)                                                          \
  X(strrchr, ucrtbase)                                                         \
  X(strstr, ucrtbase)                                                          \
  X(wcschr, ucrtbase)                                                          \
  X(wcsrchr, ucrtbase)                                                         \
  X(wcsstr, ucrtbase)                                                          \
  X(__toascii, convert)                                                        \
  X(_atoi64, convert)                                                          \
  X(_i64toa, convert)                                                          \
  X(_i64toa_s, convert)                                                        \
  X(_i64tow, convert)                                                          \
  X(_i64tow_s, convert)                                                        \
  X(_itoa, convert)                                                            \
  X(_itoa_s, convert)                                                          \
  X(_itow, convert)                                                            \
  X(_itow_s, convert)                                                          \
  X(_ltoa, convert)                                                            \
  X(_ltoa_s, convert)                                                          \
  X(_ltow, convert)                                                            \
  X(_ltow_s, convert)                                                          \
  X(_ui64toa, convert)                                                         \
  X(_ui64toa_s, convert)                                                       \
  X(_ui64tow, convert)                                                         \
  X(_ui64tow_s, convert)                                                       \
  X(_ultoa, convert)                                                           \
  X(_ultoa_s, convert)                                                         \
  X(_ultow, convert)                                                           \
  X(_ultow_s, convert)                                                         \
  X(_wcstoi64, convert)                                                        \
  X(_wcstoui64, convert)                                                       \
  X(_wtoi, convert)                                                            \
  X(_wtoi64, convert)                                                          \
  X(_wtol, convert)                                                            \
  X(atoi, convert)                                                             \
  X(atol, convert)                                                             \
  X(mbstowcs, convert)                                                         \
  X(strtol, convert)                                                           \
  X(strtoul, convert)                                                          \
  X(wcstol, convert)                                                           \
  X(wcstombs, convert)                                                         \
  X(wcstoul, convert)                                                          \
  X(_makepath_s, filesystem)                                                   \
  X(_splitpath, filesystem)                                                    \
  X(_splitpath_s, filesystem)                                                  \
  X(_wmakepath_s, filesystem)                                                  \
  X(_wsplitpath_s, filesystem)                                                 \
  X(atan, math)                                                                \
  X(atan2, math)                                                               \
  X(ceil, math)                                                                \
  X(cos, math)                                                                 \
  X(fabs, math)                                                                \
  X(floor, math)                                                               \
  X(log, math)                                                                 \
  X(pow, math)                                                                 \
  X(sin, math)                                                                 \
  X(sqrt, math)                                                                \
  X(tan, math)                                                                 \
  X(_errno, runtime)                                                           \
  X(__isascii, string)                                                         \
  X(__iscsym, string)                                                          \
  X(__iscsymf, string)                                                         \
  X(_memccpy, string)                                                          \
  X(_memicmp, string)                                                          \
  X(_stricmp, string)                                                          \
  X(_strlwr, string)                                                           \
  X(_strlwr_s, string)                                                         \
  X(_strnicmp, string)                                                         \
  X(_strnset_s, string)                                                        \
  X(_strset_s, string)                                                         \
  X(_strupr, string)                                                           \
  X(_strupr_s, string)                                                         \
  X(_wcsicmp, string)                                                          \
  X(_wcslwr, string)                                                           \
  X(_wcslwr_s, string)                                                         \
  X(_wcsnicmp, string)                                                         \
  X(_wcsnset_s, string)                                                        \
  X(_wcsset_s, string)                                                         \
  X(_wcsupr, string)                                                           \
  X(_wcsupr_s, string)                                                         \
  X(isalnum, string)                                                           \
  X(isalpha, string)                                                           \
  X(iscntrl, string)                                                           \
  X(isdigit, string)                                                           \
  X(isgraph, string)                                                           \
  X(islower, string)                                                           \
  X(isprint, string)                                                           \
  X(ispunct, string)                                                           \
  X(isspace, string)                                                           \
  X(isupper, string)                                                           \
  X(iswalnum, string)                                                          \
  X(iswalpha, string)                                                          \
  X(iswascii, string)                                                          \
  X(iswctype, string)                                                          \
  X(iswdigit, string)                                                          \
  X(iswgraph, string)                                                          \
  X(iswlower, string)                                                          \
  X(iswprint, string)                                                          \
  X(iswspace, string)                                                          \
  X(iswxdigit, string)                                                         \
  X(isxdigit, string)                                                          \
  X(memcpy_s, string)                                                          \
  X(memmove_s, string)                                                         \
  X(strcat, string)                                                            \
  X(strcat_s, string)                                                          \
  X(strcmp, string)                                                            \
  X(strcpy, string)                                                            \
  X(strcpy_s, string)                                                          \
  X(strcspn, string)                                                           \
  X(strlen, string)                                                            \
  X(strncat, string)                                                           \
  X(strncat_s, string)                                                         \
  X(strncmp, string)                                                           \
  X(strncpy, string)                                                           \
  X(strncpy_s, string)                                                         \
  X(strnlen, string)                                                           \
  X(strpbrk, string)                                                           \
  X(strspn, string)                                                            \
  X(strtok_s, string)                                                          \
  X(tolower, string)                                                           \
  X(toupper, string)                                                           \
  X(towlower, string)                                                          \
  X(towupper, string)                                                          \
  X(wcscat, string)                                                            \
  X(wcscat_s, string)                                                          \
  X(wcscmp, string)                                                            \
  X(wcscpy, string)                                                            \
  X(wcscpy_s, string)                                                          \
  X(wcscspn, string)                                                           \
  X(wcslen, string)                                                            \
  X(wcsncat, string)                                                           \
  X(wcsncat_s, string)                                                         \
  X(wcsncmp, string)                                                           \
  X(wcsncpy, string)                                                           \
  X(wcsncpy_s, string)                                                         \
  X(wcsnlen, string)                                                           \
  X(wcspbrk, string)                                                           \
  X(wcsspn, string)                                                            \
  X(wcstok_s, string)                                                          \
  X(_lfind, utility)                                                           \
  X(abs, utility)                                                              \
  X(bsearch, utility)                                                          \
  X(bsearch_s, utility)                                                        \
  X(labs, utility)                                                             \
  X(qsort, utility)                                                            \
  X(qsort_s, utility)

// ucrtbase.dll exports the function that _setjmp names only as setjmp.
#define WINCRT_SHARED_IMPORTS_RENAMED(X)                                       \
  X(_setjmp, ucrtbase, setjmp)

#endif // COMPILER_RT_LIB_WINCRT_SHARED_IMPORTS_H
