// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -emit-llvm -o - %s | FileCheck %s

// On Windows Itanium a declaration of the UCRT or the SDK is foreign and
// passed no thread pointer: one written with a Windows convention's keyword,
// or a C library function. A definition the unit compiles is never foreign.

typedef unsigned long long size_t;
int __cdecl printf(const char *, ...);
size_t strlen(const char *);
__declspec(dllimport) void __stdcall Sleep(unsigned);
int mine(int);
int __cdecl own(int x) { return x; }

int use(const char *s) {
  printf("%s", s);
  Sleep(1);
  return (int)strlen(s) + mine(1) + own(2);
}

// CHECK: define {{.*}}i32 @own(i32 noundef %x) #[[ARG:[0-9]+]]
// CHECK: declare {{.*}}i32 @printf(ptr noundef, ...) #[[FOREIGN:[0-9]+]]
// CHECK: declare {{.*}}void @Sleep(i32 noundef) #[[FOREIGN]]
// CHECK: declare {{.*}}i64 @strlen(ptr noundef) #[[LIBC:[0-9]+]]
// CHECK: declare {{.*}}i32 @mine(i32 noundef) #[[DECL:[0-9]+]]
// CHECK: attributes #[[ARG]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[FOREIGN]] = {
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
// CHECK: attributes #[[LIBC]] = {
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
// CHECK: attributes #[[DECL]] = {{{.*}} "thread-pointer-argument" }
