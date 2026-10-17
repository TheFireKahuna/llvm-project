// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -o - %s | FileCheck --check-prefix=ELF %s

/// A unit that copies the vtable of a class whose key function it does not
/// define defines the class's implicit virtual destructor, which the copy
/// names and no image exports: for a class the visibility mapping exports,
/// one whose key function is dllexport elsewhere in the image, and one whose
/// key function it imports.

#define API __attribute__((visibility("default")))

struct API Base {
  virtual ~Base();
  virtual void f();
};

struct API Mapped : Base {
  void f() override;
};

struct Exported : Base {
  __attribute__((dllexport)) void f() override;
};

struct Imported : Base {
  __attribute__((dllimport)) void f() override;
};

Mapped *m() { return new Mapped; }
Exported *e() { return new Exported; }
Imported *i() { return new Imported; }

// CHECK-DAG: @_ZTV6Mapped = linkonce_odr {{.*}} @_ZN6MappedD1Ev, {{.*}} @_ZN6MappedD0Ev
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN6MappedD1Ev(
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN6MappedD0Ev(
// CHECK-DAG: @_ZTV8Exported = linkonce_odr
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN8ExportedD1Ev(
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN8ExportedD0Ev(
// CHECK-DAG: @_ZTV8Imported = linkonce_odr
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN8ImportedD1Ev(
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN8ImportedD0Ev(

// ELF: @_ZTV6Mapped = external
// ELF-NOT: define {{.*}}@_ZN6MappedD0Ev(
