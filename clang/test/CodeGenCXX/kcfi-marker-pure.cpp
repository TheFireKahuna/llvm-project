// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefix=LINUX

/// Under the KCFI marker scheme, a pure or deleted virtual function's vtable
/// slot holds a stub that carries the slot's type and calls the runtime's
/// function, so that a call through the slot reaches the runtime's report.
/// Slots of one type share a stub.

struct Base {
  virtual void f() = 0;
  virtual void h() = delete;
  virtual void g();
};
struct Der : Base {
  void f() override;
  void h() override = delete;
};
void Base::g() {}
void Der::f() {}

// CHECK: @_ZTV4Base = {{.*}} [ptr null, ptr @_ZTI4Base, ptr @_purecall.kcfi.27ccb481, ptr @__cxa_deleted_virtual.kcfi.27ccb481, ptr @_ZN4Base1gEv]
// CHECK: @_ZTV3Der = {{.*}} [ptr null, ptr @_ZTI3Der, ptr @_ZN3Der1fEv, ptr @__cxa_deleted_virtual.kcfi.27ccb481, ptr @_ZN4Base1gEv]
// LINUX: @_ZTV4Base = {{.*}} [ptr null, ptr @_ZTI4Base, ptr @__cxa_pure_virtual.kcfi.27ccb481, ptr @__cxa_deleted_virtual.kcfi.27ccb481, ptr @_ZN4Base1gEv]

// CHECK-LABEL: define dso_local void @_Z4callP4Base(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 667726977) ]
void call(Base *p) { p->f(); }

// CHECK:      define linkonce_odr hidden void @_purecall.kcfi.27ccb481() {{.*}}comdat !kcfi_type ![[#TYPE:]] {{.*}}{
// CHECK:        call void @_purecall()
// CHECK-NEXT:   unreachable
// CHECK:      define linkonce_odr hidden void @__cxa_deleted_virtual.kcfi.27ccb481() {{.*}}comdat !kcfi_type ![[#TYPE]] {{.*}}{
// CHECK:        call void @__cxa_deleted_virtual()
// CHECK-NEXT:   unreachable
// CHECK:      ![[#TYPE]] = !{i32 667726977}
