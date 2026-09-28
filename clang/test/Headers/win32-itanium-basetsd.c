// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=C
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c89 -emit-llvm -o - %s | FileCheck %s --check-prefix=C
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - -x c++ %s | FileCheck %s --check-prefix=CXX

// A C call to one of basetsd.h's __inline helpers that is not inlined calls
// a local definition, not an external function that no library provides.

#include <basetsd.h>

unsigned long convert(void *p) { return PtrToUlong(p); }

// C: define internal i32 @PtrToUlong(
// CXX: define linkonce_odr {{.*}}i32 @PtrToUlong(
