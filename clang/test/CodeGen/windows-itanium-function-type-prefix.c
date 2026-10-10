// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=NONE
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=NONE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=NONE

// Windows Itanium and NT-POSIX give every function a KCFI type prefix with a
// marker without -fsanitize=kcfi, and no other target does.

// CHECK: define {{.*}}void @f() {{.*}}!kcfi_type
// CHECK: !{i32 4, !"function-type-prefix", i32 {{-?[0-9]+}}}
// NONE-NOT: !kcfi_type
// NONE-NOT: !"function-type-prefix"

void f(void) {}
