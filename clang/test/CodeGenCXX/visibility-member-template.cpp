// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,ELF
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,COFF

/// A specialization of a member template of a class template specialization
/// takes the visibility attribute of the member template it was instantiated
/// from, as a member function of a class template specialization does, at
/// any depth of nesting. An explicit specialization of the member template
/// does not inherit it.

#define HIDDEN __attribute__((visibility("hidden")))

template <class T> struct __attribute__((visibility("default"))) A {
  template <class U> HIDDEN int hid(U) { return 1; }
  template <class U> int plain(U) { return 2; }
  HIDDEN int fn() { return 3; }
  template <class V> struct B {
    template <class U> HIDDEN int hid(U) { return 4; }
  };
};

template <> template <class U> int A<long>::hid(U) { return 5; }

int use(A<int> &a, A<int>::B<int> &b, A<long> &l) {
  return a.hid(1) + a.plain(1) + a.fn() + b.hid(1) + l.hid(1);
}

// CHECK-DAG:  define linkonce_odr hidden noundef i32 @_ZN1AIiE3hidIiEEiT_(
// ELF-DAG:    define linkonce_odr noundef i32 @_ZN1AIiE5plainIiEEiT_(
// COFF-DAG:   define linkonce_odr dso_local noundef i32 @_ZN1AIiE5plainIiEEiT_(
// CHECK-DAG:  define linkonce_odr hidden noundef i32 @_ZN1AIiE2fnEv(
// CHECK-DAG:  define linkonce_odr hidden noundef i32 @_ZN1AIiE1BIiE3hidIiEEiT_(
// ELF-DAG:    define linkonce_odr noundef i32 @_ZN1AIlE3hidIiEEiT_(
// COFF-DAG:   define linkonce_odr dso_local noundef i32 @_ZN1AIlE3hidIiEEiT_(
