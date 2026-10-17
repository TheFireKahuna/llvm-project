// RUN: %clang_cc1 -triple x86_64-w64-mingw32 -O1 -disable-llvm-passes \
// RUN:   -emit-llvm -o - %s -DATTR="__attribute__((dllexport))" | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-w64-mingw32 -O1 -disable-llvm-passes \
// RUN:   -emit-llvm -o - %s -DATTR="__attribute__((dllimport))" | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -O1 \
// RUN:   -disable-llvm-passes -mdefault-visibility-export-mapping=all \
// RUN:   -emit-llvm -o - %s -DATTR= | FileCheck --check-prefix=COPY %s

// An available_externally VTT refers to construction vtables of internal
// linkage, which take no DLL storage class from their class. On Windows
// Itanium, an image constructs a class that another image exports, and that a
// derived class may extend, through copies of its vtable, VTT and
// construction vtables.

struct ATTR Base {
  Base() {}
  virtual void f();
};

struct ATTR Derived : virtual Base {
  Derived() {}
  virtual void g();
};

struct ATTR More : Derived {
  More() {}
  virtual void h();
};

// CHECK-DAG: @_ZTT4More = available_externally
// CHECK-DAG: @_ZTC4More0_7Derived = internal constant

// COPY-DAG: @_ZTT4More = linkonce_odr dso_local unnamed_addr constant
// COPY-DAG: @_ZTC4More0_7Derived = linkonce_odr dso_local unnamed_addr constant

void use() { More m; }
