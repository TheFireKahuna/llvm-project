// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -pic-level 2 \
// RUN:   -flto=thin -emit-llvm -o - %s | FileCheck %s

/// A C compound literal at file scope is static data, whose addresses inside
/// another image's variable bitcode names too.

extern int arr[4];
int **pp = (int *[]){&arr[1], &arr[2]};

// CHECK-DAG: !{!"/INTERIOR:arr,4"}
// CHECK-DAG: !{!"/INTERIOR:arr,8"}
