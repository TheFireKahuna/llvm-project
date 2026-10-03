// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix \
// RUN:   -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -mdefault-visibility-export-mapping=explicit -fno-auto-import -O1 \
// RUN:   -disable-llvm-passes -emit-llvm -o - %s | FileCheck --check-prefix=OPT %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=NOMAP %s

// On Windows Itanium and NT-POSIX a vtable or VTT that the visibility mapping
// exports is reached from other images through its import pointer, so a
// declaration of one is not dso_local. A class the mapping does not export,
// and a definition, stay dso_local.

#define EXPORTED __attribute__((visibility("default")))

struct EXPORTED Exported {
  Exported() {}
  virtual void f();
};

struct Plain {
  Plain() {}
  virtual void f();
};

struct __attribute__((visibility("hidden"))) Hidden {
  Hidden() {}
  virtual void f();
};

struct EXPORTED Base {
  Base() {}
  virtual void f();
};

struct EXPORTED Derived : virtual Base {
  Derived() {}
  virtual void g();
};

struct EXPORTED More : Derived {
  More() {}
  virtual void h();
};

struct EXPORTED Later {
  Later() {}
  virtual void f();
};

// CHECK-DAG: @_ZTV8Exported = external constant
// CHECK-DAG: @_ZTV5Plain = external dso_local constant
// CHECK-DAG: @_ZTV6Hidden = external hidden constant
// CHECK-DAG: @_ZTV4More = external constant
// CHECK-DAG: @_ZTT4More = external unnamed_addr constant
// CHECK-DAG: @_ZTV5Later = dso_local dllexport constant

// OPT-DAG: @_ZTV8Exported = available_externally constant
// OPT-DAG: @_ZTV5Plain = available_externally dso_local constant
// OPT-DAG: @_ZTT4More = available_externally unnamed_addr constant

// NOMAP-DAG: @_ZTV8Exported = external dso_local constant
// NOMAP-DAG: @_ZTT4More = external dso_local unnamed_addr constant

void use() {
  Exported e;
  Plain p;
  Hidden h;
  More m;
  Later l;
}

void Later::f() {}
