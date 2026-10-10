// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fno-use-cxa-atexit -emit-llvm -o - %s | FileCheck %s --check-prefix=ATEXIT

// The functions that register a destructor at exit are defined in every image
// by its startup code, so they are not imported, unlike the C++ runtime's
// thread-exit registration.

struct S {
  ~S();
};
S s;
thread_local S t;
S *use() { return &t; }

// CHECK-DAG: declare dso_local i32 @__cxa_atexit(ptr, ptr, ptr)
// CHECK-DAG: declare dllimport i32 @__cxa_thread_atexit(ptr, ptr, ptr)

// ATEXIT-DAG: declare dso_local i32 @atexit(ptr)
// ATEXIT-DAG: declare dllimport i32 @__cxa_thread_atexit(ptr, ptr, ptr)
