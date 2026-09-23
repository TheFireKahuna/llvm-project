// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fcxx-exceptions -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fcxx-exceptions -emit-obj %s -o %t.obj
// RUN: llvm-readobj --sections %t.obj | FileCheck %s --check-prefix=OBJECT

namespace std { class type_info; }
struct Incomplete;
const std::type_info &incomplete() { return typeid(Incomplete *); }

namespace {
struct Internal {};
}
const std::type_info &internal() { return typeid(Internal); }

struct __attribute__((visibility("hidden"))) External {};
const std::type_info &external() { return typeid(External); }

// The ABI's incomplete descriptors remain private. Their externally linked
// names are independently canonical, so later complete definitions can agree.
// CHECK-DAG: @_ZTIP10Incomplete = internal constant { ptr, ptr, i32, ptr } {{.*}} i32 8, ptr @_ZTI10Incomplete }, align 8, !coff.binding ![[PRIVATE:[0-9]+]]
// CHECK-DAG: @_ZTSP10Incomplete = linkonce_odr constant {{.*}} !coff.binding ![[NAME:[0-9]+]]
// CHECK-DAG: @_ZTI10Incomplete = internal constant { ptr, ptr } {{.*}} !coff.binding ![[PRIVATE]]
// CHECK-DAG: @_ZTS10Incomplete = linkonce_odr constant {{.*}} !coff.binding ![[NAME]]

// Internal language identities remain distinct. Ordinary source visibility
// alone does not grant an externally linked type a second identity.
// CHECK-DAG: @_ZTIN12_GLOBAL__N_18InternalE = internal constant {{.*}} !coff.binding ![[PRIVATE]]
// CHECK-DAG: @_ZTSN12_GLOBAL__N_18InternalE = internal constant {{.*}} !coff.binding ![[LOCALNAME:[0-9]+]]
// CHECK-DAG: @_ZTI8External = linkonce_odr constant {{.*}} !coff.binding ![[CANONICAL:[0-9]+]]
// CHECK-DAG: @_ZTS8External = linkonce_odr constant {{.*}} !coff.binding ![[NAME]]
// CHECK-DAG: ![[PRIVATE]] = !{i32 2}
// CHECK-DAG: ![[LOCALNAME]] = !{i32 4}
// CHECK-DAG: ![[CANONICAL]] = !{i32 3}
// CHECK-DAG: ![[NAME]] = !{i32 5}
// CHECK-DAG: !{i32 1, !"coff.rtti_abi", i32 2}

// Eight entity records, one eight-byte header, no COFF relocations. Binding
// records are removed by the linker and cannot root otherwise dead entities.
// OBJECT: Name: .llvm.bind
// OBJECT: RawDataSize: 48
// OBJECT: RelocationCount: 0
// OBJECT: IMAGE_SCN_LNK_INFO
// OBJECT: IMAGE_SCN_LNK_REMOVE
