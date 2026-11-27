// REQUIRES: x86-registered-target

// An object carries the time it was written only for the targets whose linker
// can link incrementally, or when asked; elsewhere its timestamp is 0.

// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -emit-obj %s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=STAMP
// RUN: %clang_cc1 -triple x86_64-unknown-uefi -emit-obj %s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=STAMP
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -emit-obj %s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=ZERO
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-obj %s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=ZERO
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -mno-incremental-linker-compatible -emit-obj %s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=ZERO
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -mincremental-linker-compatible -emit-obj %s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=STAMP

// RUN: echo nop > %t.s
// RUN: %clang -cc1as -triple x86_64-pc-windows-msvc -filetype obj %t.s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=STAMP
// RUN: %clang -cc1as -triple x86_64-w64-windows-gnu -filetype obj %t.s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=ZERO
// RUN: %clang -cc1as -triple x86_64-unknown-windows-itanium -filetype obj %t.s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=ZERO
// RUN: %clang -cc1as -triple x86_64-pc-windows-msvc -mno-incremental-linker-compatible -filetype obj %t.s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=ZERO
// RUN: %clang -cc1as -triple x86_64-w64-windows-gnu -mincremental-linker-compatible -filetype obj %t.s -o - | llvm-readobj -h - | FileCheck %s --check-prefix=STAMP

// STAMP: TimeDateStamp: {{.*}} (0x{{[1-9A-F][0-9A-F]*}})
// ZERO:  TimeDateStamp: 1970-01-01 00:00:00 (0x0)

void f(void) {}
