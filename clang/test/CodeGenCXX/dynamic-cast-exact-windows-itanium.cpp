// On Windows Itanium and NT-POSIX each image holds its own copy of a
// vague-linkage vtable, so only a vtable with strong linkage that is not a
// template instantiation keeps the exact dynamic_cast. A template
// instantiation seen through an explicit instantiation declaration loses it
// too, as another image may instantiate the vtable itself.
//
// RUN: %clang_cc1 %s -triple x86_64-unknown-windows-itanium -O1 -emit-llvm -std=c++11 -o - | FileCheck %s --check-prefixes=CHECK,PE
// RUN: %clang_cc1 %s -triple aarch64-unknown-windows-itanium -O1 -emit-llvm -std=c++11 -o - | FileCheck %s --check-prefixes=CHECK,PE
// RUN: %clang_cc1 %s -triple x86_64-pc-windows-ntposix -O1 -emit-llvm -std=c++11 -o - | FileCheck %s --check-prefixes=CHECK,PE
// RUN: %clang_cc1 %s -triple aarch64-pc-windows-ntposix -O1 -emit-llvm -std=c++11 -o - | FileCheck %s --check-prefixes=CHECK,PE
// RUN: %clang_cc1 %s -triple x86_64-unknown-linux-gnu -O1 -emit-llvm -std=c++11 -o - | FileCheck %s --check-prefixes=CHECK,UNIQUE
// RUN: %clang_cc1 %s -triple x86_64-w64-windows-gnu -O1 -emit-llvm -std=c++11 -o - | FileCheck %s --check-prefixes=CHECK,UNIQUE

struct A { virtual ~A(); };

// A weak vtable can be duplicated, so its address is insignificant.
// PE: @_ZTV1C = linkonce_odr {{.*}}unnamed_addr constant
// UNIQUE: @_ZTV1C = linkonce_odr {{(dso_local )?}}constant

// A key function: the vtable is defined once.
struct K final : A { virtual void g(); };

// CHECK-LABEL: @_Z6cast_kP1A
K *cast_k(A *a) {
  // CHECK-NOT: call {{.*}} @__dynamic_cast
  // CHECK: icmp eq ptr {{.*}}, {{.*}}@_ZTV1K
  return dynamic_cast<K *>(a);
}

// No key function: the vtable is weak.
struct C final : A { };
C *make_c() { return new C; }

// CHECK-LABEL: @_Z6cast_cP1A
C *cast_c(A *a) {
  // PE: call {{.*}} @__dynamic_cast
  // UNIQUE-NOT: call {{.*}} @__dynamic_cast
  // UNIQUE: icmp eq ptr {{.*}}, {{.*}}@_ZTV1C
  return dynamic_cast<C *>(a);
}

// A template instantiation behind an explicit instantiation declaration: the
// vtable is external here.
template <class X> struct T final : A { };
extern template struct T<int>;

// CHECK-LABEL: @_Z6cast_tP1A
T<int> *cast_t(A *a) {
  // PE: call {{.*}} @__dynamic_cast
  // UNIQUE-NOT: call {{.*}} @__dynamic_cast
  // UNIQUE: icmp eq ptr {{.*}}, {{.*}}@_ZTV1TIiE
  return dynamic_cast<T<int> *>(a);
}
