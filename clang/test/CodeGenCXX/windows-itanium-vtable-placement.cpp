// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -fno-rtti | FileCheck --check-prefix=NORTTI %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=ELF %s

// On Windows Itanium and NT-POSIX every vtable's primary address point is
// placed at offset 16 of a 64-byte line. The address point is 16 + 8k bytes
// into the vtable, k counting the vcall and vbase offsets of a class with
// virtual bases, so the vtable is aligned to 64 only when they fill whole
// lines. Every placed vtable carries a pin that is not required.

struct V { virtual void v(); };

// k = 0: align 64, with a pin that says as much. Without RTTI, vtables of
// different classes can be identical, and the pins keep the linker from
// folding them.
// CHECK-DAG: @_ZTV5Plain = {{.*}} align 64, !pin ![[K0:[0-9]+]]{{$}}
// NORTTI-DAG: @_ZTV5Plain = {{.*}} align 64, !pin
// NORTTI-DAG: @_ZTV6VBased = {{.*}} align 8, !pin
// CHECK-DAG: ![[K0]] = !{i64 16, i64 6, i64 16, i64 0}
struct Plain { virtual void f(); };
void Plain::f() {}

// k = 2, one vcall and one vbase offset: the address point is 32 bytes in.
// CHECK-DAG: @_ZTV6VBased = {{.*}} align 8, !pin ![[VB:[0-9]+]]
// CHECK-DAG: ![[VB]] = !{i64 32, i64 6, i64 16, i64 0}
struct VBased : virtual V { virtual void f(); };
void VBased::f() {}

// The construction vtable of VBased in Derived takes the same rule.
// CHECK-DAG: @_ZTC7Derived0_6VBased = {{.*}} align 8, !pin ![[VB]]{{$}}
// A VTT is not placed.
// CHECK-DAG: @_ZTT7Derived = {{.*}} align 8{{$}}
struct Derived : VBased { virtual void g(); };
void Derived::g() {}

// ELF-NOT: !pin
// ELF: @_ZTV5Plain = {{.*}} align 8{{$}}
