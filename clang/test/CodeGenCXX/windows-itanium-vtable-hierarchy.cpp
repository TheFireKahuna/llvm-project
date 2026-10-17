// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm \
// RUN:   -funique-internal-linkage-names -o - %s | FileCheck --check-prefix=HASH %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=ELF %s

// On Windows Itanium and NT-POSIX each vtable definition carries
// vtable_hierarchy metadata: the primary address point's offset into the
// global, the class's name without the _ZTS prefix, and the name of its
// primary base, so the linker lays out a class's vtables as a run. ELF has no
// such metadata.

// CHECK: @_ZTV1A = {{.*}}, !vtable_hierarchy ![[A:[0-9]+]]
struct A { virtual void f(); };
void A::f() {}

// A class's primary base is named; a non-primary base is not, since the run
// follows primary bases alone.
// CHECK: @_ZTV1B = {{.*}}, !vtable_hierarchy ![[B:[0-9]+]]
struct Other { virtual void h(); };
struct B : A, Other { virtual void g(); };
void B::g() {}

// A class in an anonymous namespace is named by its mangling. With a module
// hash (-funique-internal-linkage-names) the hash is appended, so two units'
// unrelated internal classes do not collide; without one they share a name and
// the linker leaves both out of a run.
// CHECK: @_ZTVN12_GLOBAL__N_11LE = {{.*}}, !vtable_hierarchy ![[L:[0-9]+]]
// HASH: @_ZTVN12_GLOBAL__N_11LE = {{.*}}, !vtable_hierarchy ![[L:[0-9]+]]
namespace {
struct L : A { void f() override; };
void L::f() {}
}
A *make() { return new L; }

// The address point is 16 bytes into the vtable.
// CHECK-DAG: ![[A]] = !{i64 16, !"1A", !""}
// CHECK-DAG: ![[B]] = !{i64 16, !"1B", !"1A"}
// CHECK-DAG: ![[L]] = !{i64 16, !"N12_GLOBAL__N_11LE", !"1A"}
// HASH-DAG: ![[L]] = !{i64 16, !"N12_GLOBAL__N_11LE.__uniq.{{[0-9]+}}", !"1A"}

// ELF-NOT: !vtable_hierarchy
