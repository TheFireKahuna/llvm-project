// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-hash=FNV-1a -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=PLAIN
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-hash=FNV-1a -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=MARKER
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-hash=FNV-1a -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=MARKER

/// The second type word of a virtual function, which a linker overwrites as
/// it does the type, never takes type 0 either. The type of void() salted
/// "f7za8ft.__vfn" hashes to 0 under FNV-1a; with function type prefixes the
/// word becomes 1. Without the marker there is no second word.

struct A {
  virtual void __attribute__((cfi_salt("f7za8ft"))) f();
};
void __attribute__((cfi_salt("f7za8ft"))) A::f() {}

// PLAIN-NOT:  !kcfi_vfn_type
// MARKER:     define {{.*}}void @_ZN1A1fEv({{.*}} !kcfi_vfn_type ![[#VFN:]]
// MARKER:     ![[#VFN]] = !{i32 1}
