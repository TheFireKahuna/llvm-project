// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -std=c++17 -triple aarch64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -std=c++17 -triple x86_64-pc-windows-ntposix -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -std=c++17 -triple i686-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefix=NORECORD

// A thread-local variable with vague linkage and default visibility has one
// instance on each thread along import paths, as other vague data does: it is
// not dso_local, so the back end reaches it through its record, and neither
// is its guard. Its init function stays the image's own.
//
// Another unit or image may reach a variable with an init function through a
// record that the linker writes, which names the byte that guards the
// variable's initialization by the name the Itanium ABI gives its guard: the
// variable's own guard, if it has one, and otherwise an alias of the unit's
// for ordered initialization. A variable that exports a record of its own
// names the guard there, and one that no other unit may reach needs no name.

#define API __attribute__((visibility("default")))

int f();

inline thread_local int vague = f();
inline thread_local int fixed = 1;
thread_local int strong = f();
API thread_local int marked = f();
template <class T> struct Member {
  static thread_local int value;
};
template <class T> thread_local int Member<T>::value = f();
__attribute__((visibility("hidden"))) inline thread_local int hidden = f();
static thread_local int internal = f();
inline int &local() {
  static thread_local int value = f();
  return value;
}

int use() {
  return vague + fixed + strong + marked + Member<int>::value + hidden +
         internal + local();
}

// CHECK-DAG: @vague = linkonce_odr thread_local global i32 0, comdat
// CHECK-DAG: @_ZGV5vague = linkonce_odr thread_local global i64 0, comdat
// CHECK-DAG: @fixed = linkonce_odr thread_local global i32 1, comdat
// CHECK-DAG: @strong = dso_local thread_local global i32 0
// CHECK-DAG: @marked = dso_local thread_local global i32 0
// CHECK-DAG: @_ZN6MemberIiE5valueE = linkonce_odr thread_local global i32 0, comdat
// CHECK-DAG: @_ZGVN6MemberIiE5valueE = linkonce_odr thread_local global i64 0, comdat
// CHECK-DAG: @hidden = linkonce_odr hidden thread_local global i32 0, comdat
// CHECK-DAG: @_ZL8internal = internal thread_local global i32 0
// CHECK-DAG: @_ZZ5localvE5value = linkonce_odr thread_local global i32 0, comdat
// CHECK-DAG: @_ZGVZ5localvE5value = linkonce_odr thread_local global i64 0, comdat
// CHECK-DAG: @"marked$tls" = dllexport constant {{.*}}(ptr @__tls_guard to i32)
// CHECK-DAG: @_ZGV6strong = hidden alias i8, ptr @__tls_guard
// CHECK-DAG: @_ZTH5vague = linkonce_odr dso_local alias void (), ptr @__tls_init
// CHECK-DAG: @_ZTHN6MemberIiE5valueE = linkonce_odr dso_local alias void ()
// CHECK-NOT: @_ZGV6marked =
// CHECK-NOT: @_ZGVL8internal =

// Where no record reaches another image's variable, every one is dso_local.
// NORECORD-DAG: @vague = linkonce_odr dso_local thread_local global i32 0, comdat
// NORECORD-DAG: @_ZGV5vague = linkonce_odr dso_local thread_local global i64 0, comdat
// NORECORD-DAG: @strong = dso_local thread_local global i32 0
// NORECORD-NOT: @_ZGV6strong =
