// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -S -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -S -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -S -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -S -o - %s | \
// RUN:   FileCheck --check-prefix=MINGW %s

// Windows Itanium and NT-POSIX place each data object in a section of its own
// without -fdata-sections.

// CHECK: .section .data,"dw",one_only,a
// CHECK: .section .rdata,"dr",one_only,b
// MINGW-NOT: one_only

int a = 1;
const int b = 2;
