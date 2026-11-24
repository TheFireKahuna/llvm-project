// On Windows Itanium and NT-POSIX clang states of a variable with vague
// linkage GCC's unique binding under -fgnu-unique, how it is initialized where
// that runs code, and that it is a type_info object. It, each of its names and
// its guard are not DSO local unless thread-local, hidden or dllexport.

// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -fgnu-unique -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,UNIQUE
// RUN: %clang_cc1 -triple aarch64-unknown-windows-ntposix -std=c++17 \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,PLAIN
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=ELF --implicit-check-not=dynamic-init \
// RUN:     --implicit-check-not=type-info --implicit-check-not=gnu-unique

int f();
inline int iv = 1;
inline int dyn = f();
template <class T> struct S {
  static int m;
  static int d;
};
template <class T> int S<T>::m = 2;
template <class T> int S<T>::d = f();
template struct S<long>;
inline int &local() {
  static int x = f();
  return x;
}
inline thread_local int tl = 4;
int strong = f();
__attribute__((weak)) int weak = 1;
struct A {
  virtual ~A() {}
};
struct K {
  virtual void k();
};
void K::k() {}
A a;
extern int al __attribute__((alias("iv")));
__attribute__((visibility("hidden"))) inline int hid = 5;
__attribute__((dllexport)) inline int exported = 6;
int use() { return iv + dyn + S<int>::m + S<int>::d + local() + tl + hid; }

// An inline variable, a static data member of a template, implicitly or
// explicitly instantiated, a static variable of an inline function and their
// thread-local forms are unique. A variable with one definition is not, nor
// is one that its definition makes weak.
// CHECK-DAG: @iv = linkonce_odr {{.*}}global i32 1, comdat, align 4[[U:( #[0-9]+)?]]
// CHECK-DAG: @_ZN1SIiE1mE = linkonce_odr {{.*}}global i32 2, comdat, align 4[[U]]
// CHECK-DAG: @_ZN1SIlE1mE = weak_odr {{.*}}global i32 2, comdat, align 4[[U]]
// CHECK-DAG: @tl = linkonce_odr {{.*}}thread_local global i32 4, comdat, align 4, !tls_record {{![0-9]+}}[[U]]
// CHECK-DAG: @strong = {{.*}}global i32 0, align 4{{$}}
// CHECK-DAG: @weak = weak {{.*}}global i32 1, align 4{{$}}
// CHECK-DAG: @al = alias i32, ptr @iv
// CHECK-DAG: @hid = linkonce_odr hidden global i32 5, comdat, align 4
// CHECK-DAG: @exported = weak_odr dso_local dllexport global i32 6, comdat, align 4

// A variable that the image initializes with itself, one that is initialized
// where control first passes its declaration, and neither guard, which is
// unique where its variable is.
// CHECK-DAG: @dyn = linkonce_odr global i32 0, comdat, align 4 [[UNORDERED:#[0-9]+]]
// CHECK-DAG: @_ZN1SIiE1dE = linkonce_odr global i32 0, comdat, align 4 [[UNORDERED]]
// CHECK-DAG: @_ZN1SIlE1dE = weak_odr global i32 0, comdat, align 4 [[UNORDERED]]
// CHECK-DAG: @_ZZ5localvE1x = linkonce_odr global i32 0, comdat, align 4 [[GUARDED:#[0-9]+]]
// CHECK-DAG: @_ZGVZ5localvE1x = linkonce_odr global i64 0, comdat($_ZZ5localvE1x), align 8[[U]]
// CHECK-DAG: @_ZGV3dyn = linkonce_odr global i64 0, comdat($dyn), align 8[[U]]

// The type_info object of a class with no key function, and not that of a
// class with one.
// CHECK-DAG: @_ZTI1A = linkonce_odr constant {{.*}}, comdat, align 8 [[TYPEINFO:#[0-9]+]]
// CHECK-DAG: @_ZTI1K = {{.*}}constant {{.*}}, align 8{{$}}

// UNIQUE-DAG: attributes [[UNORDERED]] = { "dynamic-init"="unordered" "gnu-unique" }
// UNIQUE-DAG: attributes [[GUARDED]] = { "dynamic-init"="guarded" "gnu-unique" }
// UNIQUE-DAG: attributes{{ #[0-9]+}} = { "gnu-unique" }
// PLAIN-DAG: attributes [[UNORDERED]] = { "dynamic-init"="unordered" }
// PLAIN-DAG: attributes [[GUARDED]] = { "dynamic-init"="guarded" }
// CHECK-DAG: attributes [[TYPEINFO]] = { "type-info" }

// ELF: @iv = linkonce_odr
