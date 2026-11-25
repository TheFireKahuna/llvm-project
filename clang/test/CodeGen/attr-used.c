// RUN: %clang_cc1 -emit-llvm -triple x86_64 %s -o - | FileCheck %s --check-prefixes=CHECK,CUSED
// RUN: %clang_cc1 -emit-llvm -triple x86_64-apple-darwin %s -o - | FileCheck %s --check-prefixes=CHECK,USED
// RUN: %clang_cc1 -emit-llvm -triple x86_64-windows-msvc %s -o - | FileCheck %s --check-prefixes=CHECK,USED

/// Windows Itanium and NT-POSIX follow ELF: the attribute keeps a definition
/// from the compiler, and only retain keeps it from the linker.
// RUN: %clang_cc1 -emit-llvm -triple x86_64-unknown-windows-itanium %s -o - | FileCheck %s --check-prefixes=CHECK,CUSED --implicit-check-not=llvm.used
// RUN: %clang_cc1 -emit-llvm -triple x86_64-pc-windows-ntposix %s -o - | FileCheck %s --check-prefixes=CHECK,CUSED --implicit-check-not=llvm.used
// RUN: %clang_cc1 -emit-llvm -triple aarch64-unknown-windows-itanium %s -o - | FileCheck %s --check-prefixes=CHECK,CUSED --implicit-check-not=llvm.used
// RUN: %clang_cc1 -emit-llvm -triple aarch64-pc-windows-ntposix %s -o - | FileCheck %s --check-prefixes=CHECK,CUSED --implicit-check-not=llvm.used

// USED:       @llvm.used =
// CUSED:      @llvm.compiler.used =
// CHECK-SAME:    @f0
// CHECK-SAME:    @f1.l0
// CHECK-SAME:    @g0
// CHECK-SAME:    @a0

int g0 __attribute__((used));

static void __attribute__((used)) f0(void) {
}

void f1(void) { 
  static int l0 __attribute__((used)) = 5225; 
}

__attribute__((used)) int a0;
void pr27535(void) { (void)a0; }
