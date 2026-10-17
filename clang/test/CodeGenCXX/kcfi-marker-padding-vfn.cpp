// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=X64
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=ARM64

/// On x86 a check of the second type word reads, in a function whose prefix
/// has none, the four bytes of padding before its marker: single-byte nops,
/// or a 4-byte nopl. A second type never takes either value. The type of
/// void() salted "s21b3b3c16.__vfn" hashes to 0x90909090, and salted
/// "s8c8a11ca.__vfn" to 0x00401F0F; each becomes the next value. AArch64 emits
/// no padding there, and keeps both.

struct A {
  virtual void __attribute__((cfi_salt("s21b3b3c16"))) f();
  virtual void __attribute__((cfi_salt("s8c8a11ca"))) g();
};
void __attribute__((cfi_salt("s21b3b3c16"))) A::f() {}
void __attribute__((cfi_salt("s8c8a11ca"))) A::g() {}

// X64:   define {{.*}}void @_ZN1A1fEv({{.*}} !kcfi_vfn_type ![[#F:]]
// X64:   define {{.*}}void @_ZN1A1gEv({{.*}} !kcfi_vfn_type ![[#G:]]
// X64:   ![[#F]] = !{i32 -1869573999}
// X64:   ![[#G]] = !{i32 4202256}
// ARM64: define {{.*}}void @_ZN1A1fEv({{.*}} !kcfi_vfn_type ![[#F:]]
// ARM64: define {{.*}}void @_ZN1A1gEv({{.*}} !kcfi_vfn_type ![[#G:]]
// ARM64: ![[#F]] = !{i32 -1869574000}
// ARM64: ![[#G]] = !{i32 4202255}
