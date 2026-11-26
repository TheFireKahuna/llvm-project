// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fms-extensions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -emit-llvm -o - %s | FileCheck %s

// A finally block, which only its parent calls, from its body or its
// cleanup, is passed the parent's thread pointer; a filter, which the
// personality calls, is passed none.

void g(void);
int h(int);

// CHECK: define dso_local void @guarded() #[[ARG:[0-9]+]]
// CHECK: define internal void @__fin_guarded({{.*}}) #[[FIN:[0-9]+]]
// CHECK: define internal i{{32|64}} @__filt_guarded({{.*}}) #[[FILT:[0-9]+]]
void guarded(void) {
  __try {
    __try { g(); } __except (h(1)) { g(); }
  } __finally { h(2); }
}

// CHECK: attributes #[[ARG]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[FIN]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[FILT]] = {
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
