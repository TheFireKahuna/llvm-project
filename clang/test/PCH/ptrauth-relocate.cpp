// RUN: %clang_cc1 -triple arm64e-apple-darwin -fptrauth-calls -fptrauth-intrinsics -std=c++23 -O2 -emit-pch -o %t %s
// RUN: %clang_cc1 -triple arm64e-apple-darwin -fptrauth-calls -fptrauth-intrinsics -std=c++23 -include-pch %t -O2 -emit-llvm -o - %s | FileCheck %s

#ifndef HEADER
#define HEADER
struct [[clang::trivially_relocatable(true)]] Owner {
  void *__ptrauth(2, 1, 123) pointer;
  Owner(Owner &&);
  ~Owner();
};
struct Nested { Owner owners[2]; };
#else
static_assert(!__builtin_is_bitwise_relocatable(Nested));
static_assert(!__builtin_is_cpp_trivially_relocatable(Nested));
// CHECK-LABEL: define{{.*}} void @transfer(
// CHECK: call i64 @llvm.ptrauth.resign(i64 {{.*}}, i32 2, i64 {{.*}}, i32 2, i64 {{.*}})
// CHECK: ret void
extern "C" void transfer(Nested *dest, Nested *src) {
  __builtin_trivially_relocate(dest, src, 1);
}
#endif
