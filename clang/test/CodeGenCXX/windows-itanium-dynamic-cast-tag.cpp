// RUN: %clang_cc1 %s -triple x86_64-unknown-windows-itanium -O1 \
// RUN:   -disable-llvm-passes -emit-llvm -o - | FileCheck %s
// RUN: %clang_cc1 %s -triple aarch64-pc-windows-ntposix -O1 \
// RUN:   -disable-llvm-passes -emit-llvm -o - | FileCheck %s

// On Windows Itanium and NT-POSIX an exact dynamic_cast to an effectively
// final class with no key function compares the vtable pointer with this
// image's copy of the vtable, then tests the tag at which every image places
// the vtable's primary address point, then confirms through the type_info,
// compared as std::type_info compares.

struct A { virtual ~A(); };
struct X { virtual ~X(); };

// The unit only casts to C: it refers to C's vtable weakly, and emits no copy.
// CHECK-DAG: @_ZTV1C = extern_weak constant { [5 x ptr] }, align 8
struct C final : A { virtual void f() {} };

// The unit constructs D: the copy the cast refers to is defined here.
// CHECK-DAG: @_ZTV1D = linkonce_odr {{.*}}constant
struct D final : A { virtual void f() {} };
D *make_d() { return new D; }

// A secondary base: the tag is on the primary address point, 16 bytes into
// the group, and the A-in-M address point is 56 bytes in.
struct M final : X, A { virtual void f() {} };

// CHECK-LABEL: define {{.*}} @_Z6cast_cP1A(
// CHECK: %[[VT:vtable[0-9]*]] = load ptr, ptr
// CHECK: icmp eq ptr %[[VT]], getelementptr (i8, ptr @_ZTV1C, i64 16)
// CHECK: dynamic_cast.tag:
// CHECK: %[[P:.*]] = ptrtoint ptr %[[VT]] to i64
// CHECK: %[[T:.*]] = and i64 %[[P]], 4088
// CHECK: icmp eq i64 %[[T]], [[#]]
// CHECK: dynamic_cast.rtti:
// CHECK: %[[RW:.*]] = getelementptr inbounds i8, ptr %[[VT]], i64 -8
// CHECK: %[[TI:.*]] = load ptr, ptr %[[RW]]
// CHECK: icmp eq ptr %[[TI]], @_ZTI1C
// CHECK: dynamic_cast.name:
// CHECK: icmp eq ptr %[[TI]], null
// CHECK: dynamic_cast.rtti.nonnull:
// CHECK: %[[NP:.*]] = getelementptr inbounds{{.*}} i8, ptr %[[TI]], i64 8
// CHECK: %[[N:.*]] = load i64, ptr %[[NP]]
// CHECK: %[[ON:.*]] = load i64, ptr getelementptr {{.*}}@_ZTI1C, i64 8)
// CHECK: icmp eq i64 %[[N]], %[[ON]]
// CHECK: dynamic_cast.flag:
// CHECK: %[[BOTH:.*]] = and i64 %[[N]], %[[ON]]
// CHECK: icmp slt i64 %[[BOTH]], 0
// CHECK: dynamic_cast.strcmp:
// CHECK: and i64 %[[N]], 9223372036854775807
// CHECK: and i64 %[[ON]], 9223372036854775807
// CHECK: call i32 @strcmp(
// CHECK: dynamic_cast.tag.end:
// CHECK: phi i1 [ true, %dynamic_cast.notnull ], [ false, %dynamic_cast.tag ],
// CHECK-SAME: [ true, %dynamic_cast.rtti ], [ false, %dynamic_cast.name ],
// CHECK-SAME: [ true, %dynamic_cast.rtti.nonnull ], [ false, %dynamic_cast.flag ],
// CHECK-SAME: [ %{{.*}}, %dynamic_cast.strcmp ]
// CHECK-NOT: @__dynamic_cast
// CHECK: ret ptr
// CHECK: declare {{.*}}i32 @strcmp(ptr{{.*}}, ptr{{.*}})
C *cast_c(A *a) { return dynamic_cast<C *>(a); }

// CHECK-LABEL: define {{.*}} @_Z6cast_dP1A(
// CHECK: icmp eq ptr %{{.*}}, getelementptr (i8, ptr @_ZTV1D, i64 16)
// CHECK: and i64 %{{.*}}, 4088
D *cast_d(A *a) { return dynamic_cast<D *>(a); }

// CHECK-LABEL: define {{.*}} @_Z6cast_mP1A(
// CHECK: icmp eq ptr %{{.*}}, getelementptr (i8, ptr @_ZTV1M, i64 56)
// CHECK: %[[P:.*]] = ptrtoint ptr %{{.*}} to i64
// CHECK: %[[Q:.*]] = sub i64 %[[P]], 40
// CHECK: and i64 %[[Q]], 4088
M *cast_m(A *a) { return dynamic_cast<M *>(a); }

// A computed tag is part of the vtable layout version.
// CHECK: !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_VTABLE_LAYOUT=1\22"}
