// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM

// A pure virtual vtable slot names the entry point that UCRT's pure-call
// handler belongs to, so _set_purecall_handler applies as it does to code
// built by MSVC. Every other Itanium target keeps __cxa_pure_virtual. A
// deleted virtual has no SDK counterpart and keeps its own name, whose
// diagnostic is more precise.

struct Abstract {
  virtual void pure() = 0;
  virtual void deleted() = delete;
  virtual ~Abstract();
};
Abstract::~Abstract() {}

// The entry point is the image's own, so it is not imported; the deleted
// one stays a C++ runtime import as before.
// CHECK: @_ZTV8Abstract = {{.*}}ptr @_purecall, ptr @__cxa_deleted_virtual
// CHECK: declare dso_local void @_purecall()
// CHECK: declare dllimport void @__cxa_deleted_virtual()
// ITANIUM: @_ZTV8Abstract = {{.*}}ptr @__cxa_pure_virtual, ptr @__cxa_deleted_virtual
