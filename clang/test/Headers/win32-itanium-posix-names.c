// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c17 -DISO_NAMES=0 -DPOSIX_NAMES=1 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=gnu17 -DISO_NAMES=1 -DPOSIX_NAMES=1 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c23 -DISO_NAMES=0 -DPOSIX_NAMES=1 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c17 -D_POSIX_C_SOURCE=200809L -DISO_NAMES=1 -DPOSIX_NAMES=1 \
// RUN:     -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -x c++ -std=c++17 -DISO_NAMES=1 -DPOSIX_NAMES=1 -fsyntax-only \
// RUN:     -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c17 -D_CRT_DECLARE_NONSTDC_NAMES=0 -DISO_NAMES=0 \
// RUN:     -DPOSIX_NAMES=0 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c17 -D_CRT_DECLARE_NONSTDC_NAMES=1 -DISO_NAMES=1 \
// RUN:     -DPOSIX_NAMES=1 -fsyntax-only -verify %s
// expected-no-diagnostics

// As with glibc, a header that ISO C defines declares the UCRT's POSIX and
// other non-standard names unless the mode is strict ISO C without a feature
// test macro, and any other header declares them in every mode. A program's own
// _CRT_DECLARE_NONSTDC_NAMES decides for every header. The names are not
// deprecated.

// The ISO C headers first, so that the headers they share with io.h, memory.h
// and sys/stat.h are first reached from them.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <io.h>
#include <memory.h>
#include <sys/stat.h>
#include <sys/types.h>

#if ISO_NAMES
int (*fileno_pointer)(void *) = fileno;
char *(*itoa_pointer)(int, char *, int) = itoa;
char *(*strdup_pointer)(char const *) = strdup;
double (*j0_pointer)(double) = j0;
#else
// A strict ISO C program may use the names itself.
int fileno, itoa, j0;
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
// C23 adds strdup and memccpy to string.h.
char *(*strdup_pointer)(char const *) = strdup;
#else
int strdup;
#endif
#endif

#if POSIX_NAMES
int (*open_pointer)(char const *, int, ...) = open;
void *(*memccpy_pointer)(void *, void const *, int, size_t) = memccpy;
int share_mode = SH_DENYNO;
off_t offset;
struct stat status;
#else
int open;
#ifdef SH_DENYNO
#error "SH_DENYNO is not declared"
#endif
#endif

// glibc defines none of these, and complex would break complex.h.
#if defined(min) || defined(max) || defined(environ) || defined(complex) ||    \
    defined(DOMAIN)
#error "the UCRT's min, max, environ, complex and DOMAIN macros stay hidden"
#endif
