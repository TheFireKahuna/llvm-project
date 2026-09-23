// RUN: %clang_cc1 -triple i686-windows-itanium -fdeclspec -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple i686-windows-itanium -fdeclspec -fcxx-exceptions -fno-rtti -emit-llvm %s -o - | FileCheck %s -check-prefix CHECK-EH-IMPORT
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s

namespace __cxxabiv1 {
class __declspec(dllexport) __fundamental_type_info {
public:
  virtual ~__fundamental_type_info();
};

__fundamental_type_info::~__fundamental_type_info() {}
}

struct __declspec(dllimport) base {
  virtual void method();
};
struct __declspec(dllexport) derived : base {
  virtual ~derived();
};
derived::~derived() {
  method();
}

void f() {
  throw base();
}

// Fundamental descriptors belong to the exported ABI runtime.
// CHECK-DAG: @_ZTIi = dso_local dllexport constant { ptr, ptr }
// CHECK-DAG: @_ZTSi = dso_local dllexport constant
// CHECK-DAG: @_ZTI7derived = dso_local dllexport constant { ptr, ptr, ptr } { ptr @"_ZTVN10__cxxabiv120__si_class_type_infoE$ap{{8|16}}", ptr @_ZTS7derived, ptr @_ZTI4base }
// CHECK-DAG: @_ZTS7derived = dso_local dllexport constant
// CHECK-DAG: @_ZTV7derived = dso_local dllexport unnamed_addr constant
// CHECK-DAG: @_ZTI4base = external dllimport constant ptr

// With RTTI disabled in the owner, exception descriptors still use the
// upstream weak-emission rule. Canonical binding remains a linker requirement.
// CHECK-EH-IMPORT: @_ZTI4base = linkonce_odr constant { ptr, ptr }
// CHECK-EH-IMPORT: @_ZTS4base = linkonce_odr constant

struct __declspec(dllimport) gatekeeper {};
struct zuul : gatekeeper {
  virtual ~zuul();
};
zuul::~zuul() {}

// Non-polymorphic imported classes have canonical RTTI in their owner too.
// CHECK-DAG: @_ZTI10gatekeeper = external dllimport constant ptr
