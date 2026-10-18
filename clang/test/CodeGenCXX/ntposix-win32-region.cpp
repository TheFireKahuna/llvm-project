// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++20 -emit-llvm -o - %s | FileCheck %s

// A Win32 region's types mangle as vendor-extended types, its function types
// take the MS x64 convention, and its records MSVC's layout.
#pragma clang win32_header

void takes(long, unsigned long, long double, wchar_t) {}
// CHECK-LABEL: define {{.*}}win64cc void @_Z5takesu12__win32_longu13__win32_ulongu19__win32_long_doubleu15__win32_wchar_t(i32 {{.*}}, i32 {{.*}}, double {{.*}}, i16 {{.*}})

struct Bits {
  char a : 4;
  int b : 4;
};
// MSVC packs a bit-field into its own declared type's unit.
static_assert(sizeof(Bits) == 8);

typedef long (*Callback)(long);
long call(Callback f) { return f(1); }
// A pointer to a region function carries its convention, which is not the
// unit's default, as a vendor qualifier.
// CHECK-LABEL: define {{.*}}win64cc {{.*}}i32 @_Z4callPU6ms_abiFu12__win32_longu12__win32_longE(
// CHECK: call win64cc {{.*}}i32 %{{.*}}(i32 {{.*}}1)
