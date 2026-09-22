// RUN: %clang_cc1 -triple arm64e-apple-darwin -fptrauth-calls -fptrauth-intrinsics -std=c++23 -O2 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple arm64e-apple-darwin -fptrauth-calls -fptrauth-intrinsics -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=UNOPT

struct [[clang::trivially_relocatable(true)]] Owner {
  void *__ptrauth(1, 1, 42) pointer;
  Owner(Owner &&);
  ~Owner();
};

// A single pointer needs neither a byte copy nor an overlap-direction test.
// CHECK-LABEL: define{{.*}} void @single(
// CHECK-SAME: ptr{{[^%]*}}[[DEST:%[^, )]+]], ptr{{[^%]*}}[[SOURCE:%[^, )]+]])
// CHECK: [[VALUE:%.*]] = load ptr, ptr [[SOURCE]], align 8
// CHECK-NOT: load
// CHECK-NOT: store
// CHECK: icmp eq ptr [[VALUE]], null
// CHECK-DAG: [[DADDR:%.*]] = ptrtoint ptr [[DEST]] to i64
// CHECK-DAG: [[SADDR:%.*]] = ptrtoint ptr [[SOURCE]] to i64
// CHECK-DAG: [[DDISC:%.*]] = {{.*}}call i64 @llvm.ptrauth.blend(i64 [[DADDR]], i64 42)
// CHECK-DAG: [[SDISC:%.*]] = {{.*}}call i64 @llvm.ptrauth.blend(i64 [[SADDR]], i64 42)
// CHECK: [[BITS:%.*]] = ptrtoint ptr [[VALUE]] to i64
// CHECK: call i64 @llvm.ptrauth.resign(i64 [[BITS]], i32 1, i64 [[SDISC]], i32 1, i64 [[DDISC]])
// CHECK-NOT: load
// CHECK: store ptr {{.*}}, ptr [[DEST]], align 8
// CHECK-NOT: store
// CHECK: ret void
// UNOPT-LABEL: define{{.*}} void @single(
// UNOPT-NOT: @llvm.mem
// UNOPT: call i64 @llvm.ptrauth.resign
// UNOPT-NOT: @llvm.mem
// UNOPT: ret void
extern "C" void single(Owner *dest, Owner *src) {
  __builtin_trivially_relocate(dest, src, 1);
}

// CHECK-LABEL: define{{.*}} void @range(
// CHECK: icmp eq i64
// CHECK: icmp ugt ptr
// CHECK: call i64 @llvm.ptrauth.resign
// CHECK: call i64 @llvm.ptrauth.resign
// CHECK: ret void
extern "C" void range(Owner *dest, Owner *src, __SIZE_TYPE__ count) {
  __builtin_trivially_relocate(dest, src, count, false);
}

struct PlainBase { unsigned value; };
struct Derived : PlainBase, Owner {
  unsigned before;
  Owner values[2];
  unsigned after;
};

// Check both address orders, including bases and arrays. Plain byte ranges
// exclude the signed fields, whose transfers use the ordinary signing helper.
// UNOPT-LABEL: define{{.*}} void @nested(
// UNOPT: icmp ugt ptr
// UNOPT: relocate.forward:
// UNOPT: call void @llvm.memmove{{.*}}i64 8
// UNOPT: call i64 @llvm.ptrauth.resign
// UNOPT: relocate.backward:
// UNOPT: call void @llvm.memmove{{.*}}i64 8
// UNOPT: call i64 @llvm.ptrauth.resign
// UNOPT: ret void
extern "C" void nested(Derived *dest, Derived *src) {
  __builtin_trivially_relocate(dest, src, 1);
}

// CHECK-LABEL: define{{.*}} void @arrays(
// CHECK: call i64 @llvm.ptrauth.resign
// CHECK: ret void
extern "C" void arrays(Owner (*dest)[2], Owner (*src)[2]) {
  __builtin_trivially_relocate(dest, src, 1);
}

struct __attribute__((packed)) Packed {
  char byte;
  void *__ptrauth(1, 1, 13) pointer;
};
// CHECK-LABEL: define{{.*}} void @packed(
// CHECK: load ptr, ptr {{.*}}, align 1
// CHECK: call i64 @llvm.ptrauth.resign
// CHECK: store ptr {{.*}}, ptr {{.*}}, align 1
// CHECK: ret void
extern "C" void packed(Packed *dest, Packed *src) {
  __builtin_trivially_relocate(dest, src, 1);
}

using SignedPointer = void *__ptrauth(2, 1, 73);
// CHECK-LABEL: define{{.*}} void @pointer(
// CHECK: load ptr, ptr
// CHECK: call i64 @llvm.ptrauth.resign(i64 {{.*}}, i32 2, i64 {{.*}}, i32 2, i64 {{.*}})
// CHECK: store ptr
// CHECK: ret void
extern "C" void pointer(SignedPointer *dest, SignedPointer *src) {
  __builtin_trivially_relocate(dest, src, 1);
}

struct NonAddressDiscriminated {
  void *__ptrauth(2, 0, 73) pointer;
};
// Moving a location-independent signature needs no authentication work.
// CHECK-LABEL: define{{.*}} void @non_address_discriminated(
// CHECK-NOT: @llvm.ptrauth
// CHECK: ret void
extern "C" void non_address_discriminated(NonAddressDiscriminated *dest,
                                           NonAddressDiscriminated *src) {
  __builtin_trivially_relocate(dest, src, 1, true);
}

// CHECK-LABEL: define{{.*}} void @zero(
// CHECK-NEXT: {{.*}}:
// CHECK-NEXT: ret void
extern "C" void zero() {
  __builtin_trivially_relocate((Owner *)nullptr, (Owner *)nullptr, 0);
}
