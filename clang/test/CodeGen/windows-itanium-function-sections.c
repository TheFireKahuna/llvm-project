// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -S -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -S -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -S -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -S -o - %s | \
// RUN:   FileCheck --check-prefix=MINGW %s

// Windows Itanium and NT-POSIX place each function in a section of its own
// without -ffunction-sections.

// CHECK: .section .text,"xr",one_only,f
// CHECK: .section .text,"xr",one_only,g
// MINGW-NOT: one_only

void f(void) {}
void g(void) {}
