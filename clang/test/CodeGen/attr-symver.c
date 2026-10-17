// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-obj -o %t.obj %s
// RUN: llvm-readobj --symbols %t.obj | FileCheck --check-prefix=COFF %s

// Each symver attribute gives a definition a versioned name through the
// assembler's .symver directive, and keeps the definition. An unused static
// function is not emitted, and names nothing.

__attribute__((symver("foo@V1"))) int foo_v1(void) { return 1; }
__attribute__((symver("foo@@V2"), symver("bar@@V2"))) int foo_v2(void) {
  return 2;
}
__attribute__((symver("data@V1"))) int data_v1 = 1;
static __attribute__((symver("unused@V1"))) int unused(void) { return 0; }

// CHECK:      module asm
// CHECK-NEXT:   ".symver foo_v1, foo@V1"
// CHECK-NEXT:   ".symver foo_v2, foo@@V2"
// CHECK-NEXT:   ".symver foo_v2, bar@@V2"
// CHECK-NEXT:   ".symver data_v1, data@V1"
// CHECK-NOT:  unused@V1
// CHECK:      @llvm.compiler.used = appending global [3 x ptr] [ptr @foo_v1, ptr @foo_v2, ptr @data_v1]

// COFF-DAG: Name: foo@V1
// COFF-DAG: Name: foo@@V2
// COFF-DAG: Name: bar@@V2
// COFF-DAG: Name: data@V1
