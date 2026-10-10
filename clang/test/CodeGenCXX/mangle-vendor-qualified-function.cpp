// RUN: %clang_cc1 -triple x86_64-linux-gnu -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-linux-gnu -emit-llvm -o - %s \
// RUN:   -fclang-abi-compat=23 | FileCheck %s --check-prefix=CLANG23

// A vendor qualifier on a function type, such as a calling convention, makes
// it a qualified type: the qualified function type and the function type
// without the qualifier are both substitution candidates, the latter first,
// as the Itanium C++ ABI has it for qualified types and as demanglers count.

typedef void (__attribute__((ms_abi)) *MP)(void *);
typedef void (*P)(void *);

// CHECK: define {{.*}} @_Z1fPU6ms_abiFvPvES2_(
// CLANG23: define {{.*}} @_Z1fPU6ms_abiFvPvES1_(
void f(MP, MP) {}

// The function type without the qualifier may already stand for it.
// CHECK: define {{.*}} @_Z1gPFvPvEPU6ms_abiS0_(
// CLANG23: define {{.*}} @_Z1gPFvPvEPU6ms_abiFvS_E(
void g(P, MP) {}

// CHECK: define {{.*}} @_Z1hPU6ms_abiFvPvEPS0_(
// CLANG23: define {{.*}} @_Z1hPU6ms_abiFvPvEPFvS_E(
void h(MP, P) {}

// CHECK: define {{.*}} @_Z1kPU6ms_abiFvPvERS1_(
// CLANG23: define {{.*}} @_Z1kPU6ms_abiFvPvERS0_(
void k(MP, void (__attribute__((ms_abi)) &)(void *)) {}
