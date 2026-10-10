// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -DCC=ms_abi -o - %s | FileCheck %s --check-prefixes=CHECK,LINUX
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 -emit-llvm \
// RUN:   -DCC=sysv_abi -o - %s | FileCheck %s --check-prefixes=CHECK,WI

// A salted function type is distinct from the unsalted one, so the Itanium
// ABI mangles the salt as a vendor qualifier on it, with the salt as its
// template argument: an identifier as a name, anything else as its bytes.

namespace std { class type_info; }

#define __cfi_salt(S) __attribute__((cfi_salt(S)))

typedef void (*plain_t)(void *);
typedef void (*salted_t)(void *) __cfi_salt("pepper");
typedef void (*other_t)(void *) __cfi_salt("salt 'n");

// Overloads that differ only by the salt.
// CHECK-DAG: define {{.*}}void @_Z1fPFvPvE(
// CHECK-DAG: define {{.*}}void @_Z1fPU8cfi_saltIX6pepperEEFvPvE(
// CHECK-DAG: define {{.*}}void @_Z1fPU8cfi_saltIJLh115ELh97ELh108ELh116ELh32ELh39ELh110EEEFvPvE(
void f(plain_t) {}
void f(salted_t) {}
void f(other_t) {}

// Instantiations over the salted and unsalted types.
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN1SIPFvPvEE1gEv(
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN1SIPU8cfi_saltIX6pepperEEFvPvEE1gEv(
template <class T> struct S {
  static void g() {}
};
void use() {
  S<plain_t>::g();
  S<salted_t>::g();
}

// A function's own type is not part of its name.
// CHECK-DAG: define {{.*}}void @_Z1hPv(
void h(void *) __cfi_salt("pepper") {}

// The qualifier follows a calling convention's.
// LINUX-DAG: define {{.*}}void @_Z1kPU6ms_abiU8cfi_saltIX6pepperEEFvPvE(
// WI-DAG: define {{.*}}void @_Z1kPU8sysv_abiU8cfi_saltIX6pepperEEFvPvE(
typedef void (__attribute__((CC)) *cc_t)(void *) __cfi_salt("pepper");
void k(cc_t) {}

// The type info of the salted type is its own.
// CHECK-DAG: @_ZTSPU8cfi_saltIX6pepperEEFvPvE = linkonce_odr
// CHECK-DAG: @_ZTSPFvPvE = linkonce_odr
const std::type_info &ti_salted() { return typeid(salted_t); }
const std::type_info &ti_plain() { return typeid(plain_t); }
