// Constant RTTI uses exact native bindings for cross-image fields.
//
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s --implicit-check-not=__typeinfo_init --implicit-check-not=global_ctors
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s --implicit-check-not=__typeinfo_init --implicit-check-not=global_ctors
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fcxx-exceptions -fno-auto-import -emit-llvm %s -o - | FileCheck %s --implicit-check-not=__typeinfo_init --implicit-check-not=global_ctors

namespace std {
class type_info;
}

struct __declspec(dllimport) imported {
  virtual void f();
};

struct local {
  virtual ~local();
};
local::~local() {}

struct derived : local {
  ~derived() override;
};
derived::~derived() {}

struct mixed : imported {
  virtual ~mixed();
};
mixed::~mixed() {}

struct __declspec(dllexport) exported {
  virtual ~exported();
};
exported::~exported() {}

template <class T> struct node : local {};

const void *use(int which) {
  node<int> n;
  switch (which) {
  case 0:
    return &typeid(n);
  case 1:
    return &typeid(imported *);
  case 2:
    return &typeid(mixed);
  case 3:
    return &typeid(int);
  }
  return &typeid(exported);
}

// Key-function ownership and normal ABI sizes are retained.
// CHECK-DAG: @_ZTI5local = dso_local constant { ptr, ptr } { ptr @"_ZTVN10__cxxabiv117__class_type_infoE$ap16", ptr @_ZTS5local }, align 8
// CHECK-DAG: @_ZTI7derived = dso_local constant { ptr, ptr, ptr } { ptr @"_ZTVN10__cxxabiv120__si_class_type_infoE$ap16", ptr @_ZTS7derived, ptr @_ZTI5local }, align 8
// CHECK-DAG: @_ZTI8imported = external dllimport constant ptr
// CHECK-DAG: @_ZTI5mixed = dso_local constant { ptr, ptr, ptr } { ptr @"_ZTVN10__cxxabiv120__si_class_type_infoE$ap16", ptr @_ZTS5mixed, ptr @_ZTI8imported }, align 8
// CHECK-DAG: @_ZTIP8imported = linkonce_odr constant { ptr, ptr, i32, ptr } { ptr @"_ZTVN10__cxxabiv119__pointer_type_infoE$ap16", ptr @_ZTSP8imported, i32 0, ptr @_ZTI8imported }, comdat, align 8
// CHECK-DAG: @_ZTI8exported = dso_local dllexport constant { ptr, ptr }
// CHECK-DAG: @_ZTS8exported = dso_local dllexport constant [10 x i8]
// CHECK-DAG: @_ZTI4nodeIiE = linkonce_odr constant { ptr, ptr, ptr }
// CHECK-DAG: @_ZTIi = external {{(dso_local )?}}constant ptr
// CHECK-DAG: @_ZTV8exported = dso_local dllexport unnamed_addr constant
