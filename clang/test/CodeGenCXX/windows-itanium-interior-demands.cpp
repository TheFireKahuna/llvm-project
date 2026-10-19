// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -pic-level 2 -flto=thin \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -pic-level 2 -flto=full \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -pic-level 2 -flto=thin \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -pic-level 2 \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=NONE %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -pic-level 2 \
// RUN:   -flto=thin -emit-llvm -o - %s | FileCheck --check-prefix=NONE %s

/// Bitcode names each address inside a variable another image may define that
/// the module's static data holds, the variable and the offset, which a joint
/// link reads before LTO writes the data: from any initializer, a constant
/// static local's included, a subobject's or a vtable's address point too.
/// The variable's own address, an address code computes, and an address inside
/// the module's own data need nothing; an object's relocations say as much.

#define API __attribute__((visibility("default")))

extern int arr[4];
struct S { int a, m; };
extern S s;
struct API K { constexpr K() {} virtual int f(); };
int local[4];

int *p = &arr[2];
int *p0 = &arr[0];
int &r = arr[1];
int *pm = &s.m;
int *lp = &local[1];
K k;
int *f() { static int *q = &arr[3]; return q; }
int *g() { return &arr[1]; }

// CHECK:     !llvm.linker.options = !{
// CHECK-DAG: !{!"/INTERIOR:_ZTV1K,16"}
// CHECK-DAG: !{!"/INTERIOR:arr,4"}
// CHECK-DAG: !{!"/INTERIOR:arr,8"}
// CHECK-DAG: !{!"/INTERIOR:arr,12"}
// CHECK-DAG: !{!"/INTERIOR:s,4"}
// CHECK-NOT: /INTERIOR:arr,0
// CHECK-NOT: /INTERIOR:local

// NONE-NOT: /INTERIOR
