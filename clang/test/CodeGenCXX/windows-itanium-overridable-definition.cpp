// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm %s -o - | FileCheck %s

// A definition of an overridable default carries the "overridable-default"
// attribute, by which the back end emits an address-take of it in import form
// (pinned by the CodeGen tests): its address is the program's one replacement,
// reached through its import pointer in every image, while calls to it stay
// direct. A function that is not an overridable default does not carry it.

typedef decltype(sizeof(0)) size_t;
extern "C" void *malloc(size_t);

// CHECK: define {{.*}}ptr @_Znw{{[my]}}(i64{{.*}}) [[NEW:#[0-9]+]]
void *operator new(size_t n) { return malloc(n); }

namespace std {
// CHECK: define {{.*}}@_ZSt{{[0-9]+}}__libcpp_verbose_abortPKcz({{.*}}) [[VA:#[0-9]+]]
void __libcpp_verbose_abort(const char *, ...) {}
} // namespace std

// CHECK: define dso_local noundef i32 @_Z6helperv() [[HLP:#[0-9]+]]
int helper() { return 7; }

// CHECK: attributes [[NEW]] = {{.*}}"overridable-default"
// CHECK: attributes [[VA]] = {{.*}}"overridable-default"
// CHECK-NOT: attributes [[HLP]] = {{.*}}"overridable-default"
