// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++17 -O1 -disable-llvm-passes -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -std=c++17 -O1 -disable-llvm-passes -emit-llvm -o - %s | FileCheck %s

// Under the owned model an imported thread-local variable and its guard are
// reached through the record of their image's place word, which the back end
// forms: the unit declares the variable and a guard that names it, and takes
// their addresses with llvm.threadlocal.address, never the loader's TLS array
// through the thread pointer.

__attribute__((dllimport)) extern thread_local int v;
int get() { return v; }

// CHECK: @v = external thread_local global i32
// CHECK: @"v$guard" = external thread_local global i8, !tls_record ![[V:[0-9]+]]
// CHECK-NOT: $tls
// CHECK-NOT: @llvm.thread.pointer
// CHECK: call ptr @llvm.threadlocal.address.p0(ptr @"v$guard")
// CHECK: call {{.*}}ptr @llvm.threadlocal.address.p0(ptr {{.*}}@v)
// CHECK: ![[V]] = !{ptr @v}
