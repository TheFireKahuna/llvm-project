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

// The runtime's standard descriptors are the same COMDAT copies every other
// image emits, neither unique nor exported.
// CHECK-DAG: @_ZTIi = linkonce_odr dso_local constant
// CHECK-DAG: @_ZTSi = linkonce_odr dso_local constant

// RTTI is a constant local copy in every image, never imported or exported;
// the runtime's type_info vtables are image-local.
// CHECK-DAG: @_ZTI7derived = linkonce_odr dso_local constant { ptr, ptr, i64, i64, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv120__si_class_type_infoE, {{i32|i64}} 2), ptr @_ZTS7derived, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, ptr @_ZTI4base }, comdat
// CHECK-DAG: @_ZTS7derived = linkonce_odr dso_local constant
// CHECK-DAG: @_ZTV7derived = dso_local dllexport unnamed_addr constant

// CHECK-DAG: @_ZTI4base = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, {{i32|i64}} 2), ptr @_ZTS4base, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}} }, comdat

// CHECK-EH-IMPORT: @_ZTI4base = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, {{i32|i64}} 2), ptr @_ZTS4base, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}} }, comdat
// CHECK-EH-IMPORT: @_ZTS4base = linkonce_odr dso_local constant

struct __declspec(dllimport) gatekeeper {};
struct zuul : gatekeeper {
  virtual ~zuul();
};
zuul::~zuul() {}

// CHECK-DAG: @_ZTI10gatekeeper = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, {{i32|i64}} 2), ptr @_ZTS10gatekeeper, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}} }, comdat
// CHECK-DAG: @_ZTS10gatekeeper = linkonce_odr dso_local constant
