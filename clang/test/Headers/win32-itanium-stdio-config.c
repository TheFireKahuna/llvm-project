// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -D_CRT_STDIO_LEGACY_WIDE_SPECIFIERS -fsyntax-only -verify %s
// expected-no-diagnostics

// The wide printf and scanf functions take ISO C's %s and %ls unless the
// project asks for the legacy behavior, which the UCRT does not allow together
// with the ISO one.

#include <stdio.h>

#ifdef _CRT_STDIO_LEGACY_WIDE_SPECIFIERS
#ifdef _CRT_STDIO_ISO_WIDE_SPECIFIERS
#error "the legacy wide specifiers exclude the ISO ones"
#endif
#elif !defined(_CRT_STDIO_ISO_WIDE_SPECIFIERS)
#error "the ISO wide specifiers are the default"
#endif
