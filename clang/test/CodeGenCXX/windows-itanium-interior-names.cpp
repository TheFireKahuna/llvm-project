// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o %t.ll %s
// RUN: FileCheck --input-file=%t.ll %s
// RUN: FileCheck --input-file=%t.ll --check-prefix=NONE %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -o - %s | FileCheck --check-prefix=ELF %s

/// A definition that another image could reach names each address inside it
/// that static data in that image may hold: each address point of a vtable,
/// counted from the start of the vtable group, and each subobject of a
/// variable that its type fixes, recursively, but nothing inside an array and
/// no reference member's storage. The names are external aliases with the
/// definition's visibility and DLL storage, so they are exported with it.

#define API __attribute__((visibility("default")))

struct API B { virtual int f() const; int b = 1; };
struct API Base2 { int x = 2; };
struct API D : B, Base2 { int f() const override; int m = 3; int arr[4] = {}; };
struct API V : virtual B { virtual int g() const; long w = 0; };
struct API W : D, V { int g() const override; };
int B::f() const { return b; }
int D::f() const { return m; }
int V::g() const { return 1; }
int W::g() const { return 2; }

D obj;
W wobj;
struct S { int a; int b; } plain;
int gi;
struct R { int i; int &r; } ref = {1, gi};
static S internal;
__attribute__((visibility("hidden"))) S hidden;
thread_local S tls;
inline S *loc() { static S s; return &s; }
S *use() { (void)internal; (void)tls; return loc(); }

// CHECK-DAG: @"obj$so8" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @obj, i64 8)
// CHECK-DAG: @"obj$so12" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @obj, i64 12)
// CHECK-DAG: @"obj$so16" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @obj, i64 16)
// CHECK-DAG: @"obj$so20" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @obj, i64 20)
// CHECK-DAG: @"wobj$so40" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @wobj, i64 40)
// CHECK-DAG: @"wobj$so48" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @wobj, i64 48)
// CHECK-DAG: @"wobj$so56" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @wobj, i64 56)
// CHECK-DAG: @"wobj$so64" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @wobj, i64 64)
// CHECK-DAG: @"plain$so4" = dso_local alias i8, getelementptr inbounds (i8, ptr @plain, i64 4)
// CHECK-DAG: @"_ZZ3locvE1s$so4" = dso_local alias i8, getelementptr inbounds (i8, ptr @_ZZ3locvE1s, i64 4)

// CHECK-DAG: @"_ZTV1B$ap16" = dso_local dllexport alias i8, getelementptr inbounds inrange(-16, 8) (i8, ptr @_ZTV1B, i64 16)
// CHECK-DAG: @"_ZTV1D$ap16" = dso_local dllexport alias i8, getelementptr inbounds inrange(-16, 8) (i8, ptr @_ZTV1D, i64 16)
// CHECK-DAG: @"_ZTV1V$ap24" = dso_local dllexport alias i8, getelementptr inbounds inrange(-24, 8) (i8, ptr @_ZTV1V, i64 24)
// CHECK-DAG: @"_ZTV1V$ap56" = dso_local dllexport alias i8, getelementptr inbounds inrange(-24, 8) (i8, ptr @_ZTV1V, i64 56)
// CHECK-DAG: @"_ZTV1W$ap24" = dso_local dllexport alias i8, getelementptr inbounds inrange(-24, 16) (i8, ptr @_ZTV1W, i64 24)
// CHECK-DAG: @"_ZTV1W$ap64" = dso_local dllexport alias i8, getelementptr inbounds inrange(-24, 8) (i8, ptr @_ZTV1W, i64 64)
// CHECK-DAG: @"_ZTV1W$ap96" = dso_local dllexport alias i8, getelementptr inbounds inrange(-24, 8) (i8, ptr @_ZTV1W, i64 96)

// NONE-NOT: @"obj$so{{24|28|32}}"
// NONE-NOT: @"ref$so
// NONE-NOT: @"{{.*}}internal{{.*}}$so
// NONE-NOT: @"hidden$so
// NONE-NOT: @"tls$so

// ELF-NOT: {{\$(so|ap)[0-9]}}
