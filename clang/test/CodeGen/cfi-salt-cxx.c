// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -fsanitize=kcfi -x c++ -o - %s | FileCheck %s

// The KCFI type of a salted function, of a function taking a salted function
// pointer and of a call through one are the same in C and C++, where the salt
// is also mangled into names.

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*salted_t)(void *) __attribute__((cfi_salt("__cxa_dtor")));

void dtor(void *p) __attribute__((cfi_salt("__cxa_dtor"))) {}
void plain(void *p) {}
void take(salted_t f) {}
void call(salted_t f, void *p) { f(p); }

#ifdef __cplusplus
}
#endif

// CHECK: define{{.*}} void @dtor({{.*}} !kcfi_type ![[DTOR:[0-9]+]]
// CHECK: define{{.*}} void @plain({{.*}} !kcfi_type ![[PLAIN:[0-9]+]]
// CHECK: define{{.*}} void @take({{.*}} !kcfi_type ![[TAKE:[0-9]+]]
// CHECK: define{{.*}} void @call(
// CHECK: call void %{{.*}}(ptr {{.*}}) [ "kcfi"(i32 -764723195) ]
// CHECK: ![[DTOR]] = !{i32 -764723195}
// CHECK: ![[PLAIN]] = !{i32 -1534530564}
// CHECK: ![[TAKE]] = !{i32 -1689558437}
