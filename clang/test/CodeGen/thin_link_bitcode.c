// REQUIRES: x86-registered-target
//
// RUN: %clang -flto=thin -fthin-link-bitcode=%t.bc -target x86_64-unknown-linux-gnu -### %s 2>&1 | FileCheck %s --check-prefix=LINKBC
//
// RUN: %clang_cc1 -o %t.newpm -flto=thin -fthin-link-bitcode=%t.newpm.nodebug -triple x86_64-unknown-linux-gnu -emit-llvm-bc -debug-info-kind=limited  %s
// RUN: llvm-bcanalyzer -dump %t.newpm | FileCheck %s
// RUN: llvm-bcanalyzer -dump %t.newpm.nodebug | FileCheck %s --check-prefix=NO_DEBUG
//
// The thin-link file is written relative to -working-directory like the main output.
// RUN: rm -rf %t.d && mkdir %t.d
// RUN: %clang_cc1 -o out.bc -flto=thin -fthin-link-bitcode=thin.bc -triple x86_64-unknown-linux-gnu -emit-llvm-bc -working-directory %t.d %s
// RUN: llvm-bcanalyzer -dump %t.d/thin.bc | FileCheck %s --check-prefix=NO_DEBUG
int main (void) {
  return 0;
}

// LINKBC: -fthin-link-bitcode=

// CHECK: COMPILE_UNIT
// NO_DEBUG-NOT: COMPILE_UNIT
