// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -emit-llvm -o - %s | FileCheck %s --check-prefix=LOCAL
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-auto-import -emit-llvm -o - %s | FileCheck %s --check-prefix=LOCAL
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s --check-prefix=SHARED
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s --check-prefix=SHARED
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -emit-llvm -o - %s | FileCheck %s --check-prefix=MINGW

// A COMDAT variable gets one copy per image here, where a shared library on
// ELF gets one per process. Under -fauto-import the definition is left
// preemptable, so that the copy the linker picks can stand for all of them;
// the default keeps each image's own. A native thread-local symbol cannot be
// imported and keeps its copy either way.

extern int ExternalData;

inline int InlineVariable = 7;

template <class T> struct Holder {
  static int Value;
};
template <class T> int Holder<T>::Value = 3;

struct Counted {
  Counted();
  int Value;
};
inline Counted &instance() {
  static Counted One;
  return One;
}

inline thread_local int ThreadLocal = 5;

inline const int InlineConstant = 9;

const int *constantAddress() { return &InlineConstant; }

int read() {
  return ExternalData + InlineVariable + Holder<int>::Value + ThreadLocal +
         InlineConstant + instance().Value;
}

// LOCAL-DAG: @ExternalData = external dso_local global i32
// LOCAL-DAG: @InlineVariable = linkonce_odr dso_local global i32 7, comdat
// LOCAL-DAG: @_ZN6HolderIiE5ValueE = linkonce_odr dso_local global i32 3, comdat
// LOCAL-DAG: @ThreadLocal = linkonce_odr dso_local thread_local global i32 5, comdat
// LOCAL-DAG: @_ZZ8instancevE3One = linkonce_odr dso_local global %struct.Counted zeroinitializer, comdat
// LOCAL-DAG: @_ZGVZ8instancevE3One = linkonce_odr dso_local global i64 0, comdat
// LOCAL-DAG: @InlineConstant = linkonce_odr dso_local constant

// The guard of a function-local static goes with the variable it guards, so
// that one image's initializer runs and the others wait on it.
// SHARED-DAG: @ExternalData = external global i32
// SHARED-DAG: @InlineVariable = linkonce_odr global i32 7, comdat
// SHARED-DAG: @_ZN6HolderIiE5ValueE = linkonce_odr global i32 3, comdat
// SHARED-DAG: @ThreadLocal = linkonce_odr dso_local thread_local global i32 5, comdat
// SHARED-DAG: @_ZZ8instancevE3One = linkonce_odr global %struct.Counted zeroinitializer, comdat
// SHARED-DAG: @_ZGVZ8instancevE3One = linkonce_odr global i64 0, comdat
// SHARED-DAG: @InlineConstant = linkonce_odr constant

// MinGW auto-imports declarations only, as before.
// MINGW-DAG: @ExternalData = external global i32
// MINGW-DAG: @InlineVariable = linkonce_odr dso_local global i32 7, comdat
// MINGW-DAG: @_ZN6HolderIiE5ValueE = linkonce_odr dso_local global i32 3, comdat
// MINGW-DAG: @ThreadLocal = linkonce_odr dso_local thread_local global i32 5, comdat
// MINGW-DAG: @_ZZ8instancevE3One = linkonce_odr dso_local global %struct.Counted zeroinitializer, comdat
// MINGW-DAG: @_ZGVZ8instancevE3One = linkonce_odr dso_local global i64 0, comdat
// MINGW-DAG: @InlineConstant = linkonce_odr dso_local constant
