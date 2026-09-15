// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fexceptions -fcxx-exceptions -exception-model=seh -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fexceptions -fcxx-exceptions -exception-model=seh -emit-llvm -o - %s | FileCheck %s
// RUN: %clang --target=x86_64-pc-windows-ntposix -fexceptions -fcxx-exceptions -S -emit-llvm -o - %s | FileCheck %s
// RUN: %clang --target=aarch64-pc-windows-ntposix -fexceptions -fcxx-exceptions -S -emit-llvm -o - %s | FileCheck %s

#ifndef __SEH__
#error ntposix must select the SEH exception ABI by default
#endif

struct Guard { ~Guard(); };
extern void may_throw();

// CHECK-LABEL: define{{.*}} @cleanup(
// CHECK-SAME: personality ptr @__gxx_personality_seh0
// CHECK: invoke{{.*}} @_Z9may_throwv
// CHECK: landingpad
// CHECK: call{{.*}} @_ZN5GuardD1Ev
extern "C" void cleanup() {
  Guard guard;
  may_throw();
}
