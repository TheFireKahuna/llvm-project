// RUN: %clang_cc1 -triple %ms_abi_triple -fms-extensions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -emit-llvm -o - %s | FileCheck %s

// A function suppressed as a Control Flow Guard target carries the
// "guard_suppress" attribute where it is defined.

__declspec(guard(suppress)) void declared(void);
__declspec(guard(suppress)) void suppressed(void) { declared(); }
void plain(void) {}

// CHECK: define {{.*}}@suppressed(){{.*}} #[[SUPPRESS:[0-9]+]]
// CHECK: declare {{.*}}@declared(){{.*}} #[[DECL:[0-9]+]]
// CHECK: define {{.*}}@plain(){{.*}} #[[PLAIN:[0-9]+]]
// CHECK: attributes #[[SUPPRESS]] = {{{.*}}"guard_suppress"
// CHECK-NOT: "guard_suppress"
