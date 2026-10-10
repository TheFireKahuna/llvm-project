// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-windows-itanium \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -std=c++17 -triple aarch64-unknown-windows-itanium \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-windows-itanium \
// RUN:   -fclang-abi-compat=23 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=MSVC
// RUN: %clang_cc1 -std=c++17 -triple x86_64-scei-windows-itanium \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefix=MSVC

// With the Itanium C++ ABI on Windows, a class that is non-trivial for the
// purposes of calls is passed and returned indirectly, and the caller destroys
// a parameter, as on other Itanium targets. On x86-64, -fclang-abi-compat=23
// restores MSVC's rules, which decide by the copy constructor alone, and the
// SCEI flavour of Windows Itanium keeps them.

// Trivial copy constructor, non-trivial destructor.
struct A {
  int i;
  ~A();
};

// Trivial copy constructor and destructor, non-trivial move constructor.
struct B {
  int i;
  B(const B &) = default;
  B(B &&);
};

// Deleted copy constructor, trivial move constructor.
struct C {
  int i;
  C(const C &) = delete;
  C(C &&) = default;
};

void pa(A) {}
// ITANIUM-LABEL: define dso_local void @_Z2pa1A(ptr {{.*}}%0)
// ITANIUM-NOT:     call
// ITANIUM:         ret void
// MSVC-LABEL:    define dso_local void @_Z2pa1A(i32 %.coerce)
// MSVC:            call void @_ZN1AD1Ev(

void pb(B) {}
// ITANIUM-LABEL: define dso_local void @_Z2pb1B(ptr {{.*}}%0)
// MSVC-LABEL:    define dso_local void @_Z2pb1B(i32 %.coerce)

void pc(C) {}
// ITANIUM-LABEL: define dso_local void @_Z2pc1C(i{{32|64}} %.coerce)
// MSVC-LABEL:    define dso_local void @_Z2pc1C(ptr {{.*}}%0)

A ra() { return {}; }
// ITANIUM-LABEL: define dso_local void @_Z2rav(ptr {{.*}}sret(%struct.A) align 4 %agg.result)
// MSVC-LABEL:    define dso_local i32 @_Z2rav()
