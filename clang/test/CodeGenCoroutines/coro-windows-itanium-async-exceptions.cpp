// REQUIRES: x86-registered-target, aarch64-registered-target
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -std=c++20 -O2 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -std=c++20 -O2 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -std=c++20 -O2 -S -o /dev/null %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -std=c++20 -O2 -S -o /dev/null %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -std=c++20 -S -o /dev/null %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -std=c++20 -S -o /dev/null %s

// Under -fasync-exceptions a coroutine is covered as any function is. A
// suspend ends the EH scopes that are open across it, and a resume or a
// destroy, which enters the coroutine past the markers that began them,
// begins them again.

#include "Inputs/coroutine.h"

struct task {
  struct promise_type {
    task get_return_object() noexcept;
    std::suspend_never initial_suspend() noexcept;
    std::suspend_never final_suspend() noexcept;
    void return_void() noexcept;
    void unhandled_exception() noexcept;
  };
};
struct A {
  A() noexcept;
  ~A();
};
extern int *p;

// The ramp constructs the object, and ends its scope and the scopes around it
// where it suspends: it returns with the object alive.
// CHECK-LABEL: define dso_local {{.*}} @_Z5alivev()
// CHECK:         store i32 1, ptr
// CHECK-NEXT:    invoke void @_ZN1AC1Ev(
// CHECK:         invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[A:.*]]
// CHECK:         invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[A]]
// CHECK:         invoke void @llvm.seh.try.end()
// CHECK-NOT:     store i32 2, ptr
// CHECK:       [[A]]:
// CHECK-NEXT:    cleanuppad within none []
// CHECK-NEXT:    call void @_ZN1AD1Ev(
//
// A resume begins the object's scope ahead of the first instruction it goes
// on with. The instruction after the object's destruction is in the try
// block's scope alone.
// CHECK-LABEL: define internal void @_Z5alivev.resume(
// CHECK:         invoke void @llvm.seh.try.begin()
// CHECK-NEXT:            to label %[[TRY:.*]] unwind label %[[LPAD:.*]]
// CHECK:       [[TRY]]:
// CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %[[RESUMED:.*]] unwind label %[[A:.*]]
// CHECK:       [[RESUMED]]:
// CHECK-NEXT:    load ptr, ptr @p
// CHECK-NEXT:    store i32 2, ptr
// CHECK-NEXT:    invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %[[DESTROY:.*]] unwind label %[[A]]
// CHECK:       [[DESTROY]]:
// CHECK-NEXT:    invoke void @_ZN1AD1Ev(
// CHECK-NEXT:            to label %[[DESTROYED:.*]] unwind label %[[LPAD]]
// CHECK:       [[DESTROYED]]:
// CHECK-NEXT:    load ptr, ptr @p
// CHECK-NEXT:    store i32 3, ptr
// CHECK:       [[A]]:
// CHECK-NEXT:    cleanuppad within none []
// CHECK-NEXT:    call void @_ZN1AD1Ev(
//
// So does a destroy, which ends the scope ahead of the destructor's call.
// CHECK-LABEL: define internal void @_Z5alivev.destroy(
// CHECK:         invoke void @llvm.seh.try.begin()
// CHECK-NEXT:            to label %[[TRY:.*]] unwind label %[[LPAD:.*]]
// CHECK:       [[TRY]]:
// CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %[[DESTROYED:.*]] unwind label %[[A:.*]]
// CHECK:       [[DESTROYED]]:
// CHECK-NEXT:    invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[A]]
// CHECK:         invoke void @_ZN1AD1Ev(
// CHECK-NEXT:            to label %{{.*}} unwind label %[[LPAD]]
task alive() {
  *p = 1;
  {
    A a;
    co_await std::suspend_always{};
    *p = 2;
  }
  *p = 3;
}

// Two scopes that are each open across a suspend are no more inside one
// another than they are in a function that does not suspend: this compiles.
struct Awaiter {
  ~Awaiter();
  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<>) noexcept;
  void await_resume() noexcept;
};
void may();
task siblings() {
  co_await Awaiter();
  may();
  co_await Awaiter();
}
