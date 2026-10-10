// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o %t.ll %s
// RUN: FileCheck --input-file=%t.ll %s
// RUN: FileCheck --input-file=%t.ll --check-prefix=NONE %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | \
// RUN:   FileCheck %s

/// A C variable of structure type that another image could reach, a
/// tentative definition included, names its members, nested ones too, but not
/// a bit-field, an array element or anything of an internal variable.

struct In { char c; int i; };
struct S { int a; struct In in; int arr[3]; int bits : 3; int after; };
struct S s = {0};
struct S tentative;
static struct S st;
struct S *use(void) { return &st; }

// CHECK-DAG: @"s$so4" = dso_local alias i8, getelementptr inbounds (i8, ptr @s, i64 4)
// CHECK-DAG: @"s$so8" = dso_local alias i8, getelementptr inbounds (i8, ptr @s, i64 8)
// CHECK-DAG: @"s$so12" = dso_local alias i8, getelementptr inbounds (i8, ptr @s, i64 12)
// CHECK-DAG: @"s$so28" = dso_local alias i8, getelementptr inbounds (i8, ptr @s, i64 28)
// CHECK-DAG: @"tentative$so28" = dso_local alias i8, getelementptr inbounds (i8, ptr @tentative, i64 28)

// NONE-NOT: @"s$so{{16|20|24}}"
// NONE-NOT: @"st$so
