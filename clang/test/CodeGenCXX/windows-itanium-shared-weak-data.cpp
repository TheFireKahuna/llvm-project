// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -fdeclspec -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s | FileCheck %s --check-prefix=MINGW

// A mark that carries a COMDAT variable across the shared-library boundary
// asks for the one instance per program that a shared library on ELF gets,
// so the definition is left preemptable and the linker binds every copy to
// one of them. An unmarked variable is this image's own, and so is one marked
// with __declspec(dllexport), whose consumers see __declspec(dllimport) and
// never emit a copy to begin with.

#define API __attribute__((visibility("default")))

API inline int InlineVariable = 7;

template <class T> struct API Holder {
  static int Value;
};
template <class T> int Holder<T>::Value = 3;

struct Counted {
  Counted();
  int Value;
};
API inline Counted &instance() {
  static Counted One;
  return One;
}

API inline const int InlineConstant = 9;

inline int Unmarked = 11;

__declspec(dllexport) inline int Spelled = 13;

const int *constantAddress() { return &InlineConstant; }

int read() {
  return InlineVariable + Holder<int>::Value + instance().Value + Unmarked +
         Spelled;
}

// The guard goes with the variable it guards, so that one image's initializer
// runs and the others wait on it.
// CHECK-DAG: @InlineVariable = linkonce_odr dllexport global i32 7, comdat
// CHECK-DAG: @_ZN6HolderIiE5ValueE = linkonce_odr dllexport global i32 3, comdat
// CHECK-DAG: @_ZZ8instancevE3One = linkonce_odr dllexport global %struct.Counted zeroinitializer, comdat
// CHECK-DAG: @_ZGVZ8instancevE3One = linkonce_odr dllexport global i64 0, comdat
// CHECK-DAG: @InlineConstant = linkonce_odr dllexport constant i32 9
// CHECK-DAG: @Unmarked = linkonce_odr dso_local global i32 11, comdat
// CHECK-DAG: @Spelled = weak_odr dso_local dllexport global i32 13, comdat

// Other COFF targets keep each image's copy.
// MINGW-DAG: @InlineVariable = linkonce_odr dso_local dllexport global i32 7, comdat
// MINGW-DAG: @_ZN6HolderIiE5ValueE = linkonce_odr dso_local dllexport global i32 3, comdat
// MINGW-DAG: @_ZZ8instancevE3One = linkonce_odr dso_local dllexport global %struct.Counted zeroinitializer, comdat
// MINGW-DAG: @_ZGVZ8instancevE3One = linkonce_odr dso_local dllexport global i64 0, comdat
