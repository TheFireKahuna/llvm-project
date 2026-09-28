// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c89 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -fms-compatibility \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,MSCOMPAT
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c99 -fgnu89-inline -fgnuc-version=4.2.1 -emit-llvm -o - %s \
// RUN:   | FileCheck %s

// In C, the UCRT functions defined __inline without extern, which the UCRT
// does not export, are local definitions wherever a call is not inlined. The
// stdio option functions, which the compiler runtime defines, stay external
// even when a header that the other functions come from includes them first.
// Under GNU89 inline semantics, as in C89, no unit defines them or the
// __forceinline ctype helpers, so that units that include the headers link
// together.

#include <wchar.h>
#include <ctype.h>
#include <fenv.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <tchar.h>

// CHECK-DAG: define internal i32 @sprintf(
// CHECK-DAG: define internal float @__ucrt_mathf(
// CHECK-DAG: define internal void @_freea(
// CHECK-DAG: define internal ptr @wmemset(
// CHECK-DAG: define internal i32 @_chvalidchk_l(
// CHECK-DAG: define internal i32 @feraiseexcept(
// MSCOMPAT-DAG: define internal i64 @_tclen(
// CHECK-DAG: declare dso_local ptr @__local_stdio_printf_options()
// CHECK-NOT: define {{.*}}@__ascii_tolower(
int use(char *buffer, void *memory, wchar_t *wide) {
  _freea(memory);
  wmemset(wide, L'x', 2);
#if !__STDC__
  (void)_tclen(buffer);
#endif
  (void)__local_stdio_printf_options();
  return sprintf(buffer, "%d", 1) + (int)__ucrt_mathf(1.0f) +
         _chvalidchk_l('a', 1) + feraiseexcept(0) + __ascii_tolower('A');
}
