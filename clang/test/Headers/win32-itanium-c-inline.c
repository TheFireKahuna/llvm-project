// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:     -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium \
// RUN:     -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c89 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:     -fms-extensions \
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:     -fms-extensions \
// In C, the functions that the UCRT and the Windows SDK define __inline
// without extern, which no library exports, are discardable ODR definitions
// wherever a call is not inlined, as MSVC emits them, so an image keeps one of
// each, whatever the inline semantics of the dialect. A static variable of
// such a function has no linkage in C. The __forceinline ctype helpers are
// always inlined.
// CHECK-DAG: @__local_stdio_printf_options._OptionsStorage = internal global i64 0
// CHECK-DAG: define linkonce_odr {{.*}}i32 @sprintf(
// CHECK-DAG: define linkonce_odr {{.*}}float @__ucrt_mathf(
// CHECK-DAG: define linkonce_odr {{.*}}void @_freea(
// CHECK-DAG: define linkonce_odr {{.*}}ptr @wmemset(
// CHECK-DAG: define linkonce_odr {{.*}}i32 @_chvalidchk_l(
// CHECK-DAG: define linkonce_odr {{.*}}i32 @feraiseexcept(
// CHECK-DAG: define linkonce_odr {{.*}}i32 @PtrToUlong(
// CHECK-DAG: define linkonce_odr {{.*}}ptr @__local_stdio_printf_options()
// MSCOMPAT-DAG: define linkonce_odr {{.*}}i64 @_tclen(
