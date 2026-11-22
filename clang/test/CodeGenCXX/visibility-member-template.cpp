// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,ELF
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,COFF

/// A specialization of a member template of a class template specialization,
/// whether a function, variable or class template, takes the visibility
/// attribute of the member template it was instantiated from, as a member
/// function of a class template specialization does, at any depth of nesting.
/// An explicit specialization of the member template does not inherit it.

#define HIDDEN __attribute__((visibility("hidden")))

template <class T> struct __attribute__((visibility("default"))) A {
  template <class U> HIDDEN int hid(U) { return 1; }
  template <class U> int plain(U) { return 2; }
  HIDDEN int fn() { return 3; }
  template <class V> struct B {
    template <class U> HIDDEN int hid(U) { return 4; }
  };
  template <class U> HIDDEN static inline int var = 6;
  template <class U> struct HIDDEN C {
    int get() { return 7; }
  };
};

template <> template <class U> int A<long>::hid(U) { return 5; }
template <> template <class U> struct A<long>::C {
  int get() { return 8; }
};

int use(A<int> &a, A<int>::B<int> &b, A<long> &l, A<int>::C<int> &c,
        A<long>::C<int> &lc) {
  return a.hid(1) + a.plain(1) + a.fn() + b.hid(1) + l.hid(1) +
         A<int>::var<int> + c.get() + lc.get();
}

// CHECK-DAG:  @_ZN1AIiE3varIiEE = linkonce_odr hidden global i32 6
// CHECK-DAG:  define linkonce_odr hidden noundef i32 @_ZN1AIiE1CIiE3getEv(
// ELF-DAG:    define linkonce_odr noundef i32 @_ZN1AIlE1CIiE3getEv(
// COFF-DAG:   define linkonce_odr dso_local noundef i32 @_ZN1AIlE1CIiE3getEv(
// CHECK-DAG:  define linkonce_odr hidden noundef i32 @_ZN1AIiE3hidIiEEiT_(
// ELF-DAG:    define linkonce_odr noundef i32 @_ZN1AIiE5plainIiEEiT_(
// COFF-DAG:   define linkonce_odr dso_local noundef i32 @_ZN1AIiE5plainIiEEiT_(
// CHECK-DAG:  define linkonce_odr hidden noundef i32 @_ZN1AIiE2fnEv(
// CHECK-DAG:  define linkonce_odr hidden noundef i32 @_ZN1AIiE1BIiE3hidIiEEiT_(
// ELF-DAG:    define linkonce_odr noundef i32 @_ZN1AIlE3hidIiEEiT_(
// COFF-DAG:   define linkonce_odr dso_local noundef i32 @_ZN1AIlE3hidIiEEiT_(
