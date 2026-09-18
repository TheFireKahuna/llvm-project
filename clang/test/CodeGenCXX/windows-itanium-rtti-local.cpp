// On Windows Itanium and NTPOSIX every image carries its own copy of every
// type_info it uses, including the standard library's, and the runtime's
// type_info classes are linked into every image. RTTI is therefore constant
// data with no cross-DLL pointers: nothing is imported, nothing is exported,
// no field needs a runtime pseudo-relocation or an initializer.
//
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s --implicit-check-not=__typeinfo_init --implicit-check-not="dllimport constant" --implicit-check-not=global_ctors
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s --implicit-check-not=__typeinfo_init --implicit-check-not="dllimport constant" --implicit-check-not=global_ctors
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fcxx-exceptions -fno-auto-import -emit-llvm %s -o - | FileCheck %s --implicit-check-not=__typeinfo_init --implicit-check-not="dllimport constant" --implicit-check-not=global_ctors

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

// Every descriptor is a local, constant, linkonce copy whose vtable pointer
// is a link-time constant, whatever the class's DLL storage.
// CHECK-DAG: @_ZTVN10__cxxabiv117__class_type_infoE = external dso_local global
// CHECK-DAG: @_ZTVN10__cxxabiv120__si_class_type_infoE = external dso_local global
// CHECK-DAG: @_ZTVN10__cxxabiv119__pointer_type_infoE = external dso_local global
// CHECK-DAG: @_ZTVN10__cxxabiv123__fundamental_type_infoE = external dso_local global
// CHECK-DAG: @_ZTI5local = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 2), ptr @_ZTS5local, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}} }, comdat, align 8
// CHECK-DAG: @_ZTS5local = linkonce_odr dso_local constant [7 x i8] c"5local\00", comdat
// CHECK-DAG: @_ZTI7derived = linkonce_odr dso_local constant { ptr, ptr, i64, i64, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv120__si_class_type_infoE, i64 2), ptr @_ZTS7derived, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, ptr @_ZTI5local }, comdat, align 8
// CHECK-DAG: @_ZTI8imported = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 2), ptr @_ZTS8imported, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}} }, comdat, align 8
// CHECK-DAG: @_ZTI5mixed = linkonce_odr dso_local constant { ptr, ptr, i64, i64, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv120__si_class_type_infoE, i64 2), ptr @_ZTS5mixed, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, ptr @_ZTI8imported }, comdat, align 8
// CHECK-DAG: @_ZTIP8imported = linkonce_odr dso_local constant { ptr, ptr, i64, i64, i32, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv119__pointer_type_infoE, i64 2), ptr @_ZTSP8imported, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, i32 0, ptr @_ZTI8imported }, comdat, align 8
// CHECK-DAG: @_ZTI8exported = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 2), ptr @_ZTS8exported, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}} }, comdat, align 8
// CHECK-DAG: @_ZTS8exported = linkonce_odr dso_local constant [10 x i8] c"8exported\00", comdat
// CHECK-DAG: @_ZTI4nodeIiE = linkonce_odr dso_local constant { ptr, ptr, i64, i64, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv120__si_class_type_infoE, i64 2), ptr @_ZTS4nodeIiE, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, ptr @_ZTI5local }, comdat, align 8

// The standard library's descriptors are emitted here too rather than
// imported from the runtime.
// CHECK-DAG: @_ZTIi = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv123__fundamental_type_infoE, i64 2), ptr @_ZTSi, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}} }, comdat, align 8
// CHECK-DAG: @_ZTSi = linkonce_odr dso_local constant [2 x i8] c"i\00", comdat

// The vtable of an exported class is still exported; only its RTTI is not.
// CHECK-DAG: @_ZTV8exported = dso_local dllexport unnamed_addr constant
