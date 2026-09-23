// Standard Itanium layouts, with canonical binding kept out of the ABI object.
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

// CHECK-DAG: @_ZTI6Shared = {{.*}}constant { ptr, ptr } { ptr @"_ZTVN10__cxxabiv117__class_type_infoE$ap16", ptr @_ZTS6Shared }, align 8, !coff.binding ![[RTTI:[0-9]+]]
// CHECK-DAG: @_ZTI4NodeIiE = linkonce_odr constant { ptr, ptr, ptr } { ptr @"_ZTVN10__cxxabiv120__si_class_type_infoE$ap16", ptr @_ZTS4NodeIiE, ptr @_ZTI6Shared }, comdat, align 8, !coff.binding ![[RTTI]]
// CHECK-DAG: @_ZTIP6Shared = linkonce_odr constant { ptr, ptr, i32, ptr } { ptr @"_ZTVN10__cxxabiv119__pointer_type_infoE$ap16", ptr @_ZTSP6Shared, i32 0, ptr @_ZTI6Shared }, comdat, align 8, !coff.binding ![[RTTI]]
// CHECK-DAG: ![[RTTI]] = !{i32 3}
// CHECK-DAG: !{i32 1, !"coff.rtti_abi", i32 2}

// ELF: @_ZTI6Shared = {{(dso_local )?}}constant { ptr, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 2), ptr @_ZTS6Shared }, align 8
