// On Windows Itanium and NTPOSIX every image carries its own copy of a type's
// RTTI, so the type_info stores a 128-bit xxh3 hash of the mangled type name
// after the name field, and the library compares that instead of addresses or
// strings. The value depends only on the mangled name, so two images agree on
// it without ever sharing a symbol.
//
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -emit-llvm -DDLL %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o - | FileCheck %s --check-prefix=ELF

namespace std {
class type_info;
}

#ifdef DLL
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

struct EXPORT Shared {
  virtual ~Shared();
};
Shared::~Shared() {}

template <class T> struct Node : Shared {};

const void *use(int which) {
  Node<int> node;
  return which ? static_cast<const void *>(&typeid(node))
               : static_cast<const void *>(&typeid(Shared *));
}

// The identity follows the name only: the same for an exported and a plain
// class, the same in every TU, and pinned to the xxh3-128 of "6Shared".
// CHECK-DAG: @_ZTI6Shared = linkonce_odr dso_local constant { ptr, ptr, i64, i64 } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 2), ptr @_ZTS6Shared, i64 -5541181936768509243, i64 -1307411804926046874 }, comdat
// CHECK-DAG: @_ZTI4NodeIiE = linkonce_odr dso_local constant { ptr, ptr, i64, i64, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv120__si_class_type_infoE, i64 2), ptr @_ZTS4NodeIiE, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, ptr @_ZTI6Shared }, comdat
// CHECK-DAG: @_ZTIP6Shared = linkonce_odr dso_local constant { ptr, ptr, i64, i64, i32, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv119__pointer_type_infoE, i64 2), ptr @_ZTSP6Shared, i64 {{-?[0-9]+}}, i64 {{-?[0-9]+}}, i32 0, ptr @_ZTI6Shared }, comdat

// Other Itanium targets keep the two-field layout.
// ELF: @_ZTI6Shared = {{(dso_local )?}}constant { ptr, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 2), ptr @_ZTS6Shared }, align 8
