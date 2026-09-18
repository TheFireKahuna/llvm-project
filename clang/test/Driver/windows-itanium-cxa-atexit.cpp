// RUN: %clang -target x86_64-unknown-windows-itanium -S -emit-llvm %s -o - | FileCheck %s --check-prefix=CXA
// RUN: %clang -target x86_64-pc-windows-ntposix -S -emit-llvm %s -o - | FileCheck %s --check-prefix=CXA
// RUN: %clang -target x86_64-unknown-windows-itanium -fno-use-cxa-atexit -S -emit-llvm %s -o - | FileCheck %s --check-prefix=ATEXIT
// RUN: %clang -target x86_64-pc-windows-msvc -S -emit-llvm %s -o - | FileCheck %s --check-prefix=ATEXIT

struct Object {
  ~Object();
} object;

// CXA: call i32 @__cxa_atexit({{.*}}@__dso_handle
// ATEXIT: call i32 @atexit(
