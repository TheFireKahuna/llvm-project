// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -fno-function-type-prefix -D_DLL \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -fno-function-type-prefix -D_DLL \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -fno-function-type-prefix \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=STATIC \
// RUN:       --implicit-check-not=dllimport

// The functions that the Visual C++ runtime would provide come from the
// UCRT's DLL, so under _DLL they are imported as the UCRT's own are.

#include <excpt.h>
#include <vcruntime_string.h>

char *find(char *s) { return strchr(s, 'a'); }
void *handler(void) { return (void *)&__C_specific_handler; }

// CHECK-DAG: declare dllimport ptr @strchr(
// CHECK-DAG: declare dllimport {{.*}}i32 @__C_specific_handler(
// STATIC-DAG: declare {{.*}}ptr @strchr(
// STATIC-DAG: declare {{.*}}i32 @__C_specific_handler(
