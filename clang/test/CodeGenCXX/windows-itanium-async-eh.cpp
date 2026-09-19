// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -emit-llvm -o - %s | FileCheck --check-prefix=SYNC %s

// Under -fasync-exceptions the Itanium personality on SEH gets the same
// scope markers as MSVC's C++ handler: the lifetime of an object and the
// extent of a try block with a catch (...) are bracketed by intrinsics that
// the backend turns into call-site ranges, so that a hardware exception
// raised by any instruction in them reaches the right landing pad.

struct T { T(); ~T(); };
void g();

// CHECK-LABEL: define dso_local noundef i32 @_Z1fPi(
// CHECK-SAME: personality ptr @__gxx_personality_seh0
int f(int *p) {
  int r = 0;
  // CHECK: invoke void @llvm.seh.try.begin()
  try {
    // CHECK: invoke {{.*}} @_ZN1TC1Ev(
    // CHECK: invoke void @llvm.seh.scope.begin()
    T t;
    // CHECK: store i32 1, ptr
    *p = 1;
    // CHECK: invoke void @_Z1gv()
    g();
    // CHECK: invoke void @llvm.seh.scope.end()
    // CHECK: call void @_ZN1TD1Ev(
    // CHECK: invoke void @llvm.seh.try.end()
  } catch (...) {
    r = 1;
  }
  // CHECK: store i32 2, ptr
  *p = 2;
  return r;
}

// An object outside any try is bracketed too: a fault during its lifetime
// runs its destructor on the way out.
// CHECK-LABEL: define dso_local void @_Z1hPi(
void h(int *p) {
  // CHECK: invoke void @llvm.seh.scope.begin()
  T t;
  // CHECK: store i32 3, ptr
  *p = 3;
  // CHECK: invoke void @llvm.seh.scope.end()
}

// A typed catch alone gets no try markers: it cannot take a structured
// exception, so there is nothing to describe.
// CHECK-LABEL: define dso_local noundef i32 @_Z1kPi(
// CHECK-NOT: llvm.seh.try.begin
// CHECK: ret i32
int k(int *p) {
  try {
    *p = 4;
    g();
  } catch (int) {
    return 1;
  }
  return 0;
}

// CHECK: !{i32 2, !"eh-asynch", i32 1}

// SYNC-NOT: llvm.seh.
// SYNC-NOT: eh-asynch
