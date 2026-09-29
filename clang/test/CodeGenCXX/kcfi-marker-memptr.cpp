// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,CHECKS,X64
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++20 -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,CHECKS,A64
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,NOCHECKS

/// Under the KCFI marker scheme, every function that can occupy a vtable slot
/// carries a second type, its function type salted "__vfn", which does not
/// depend on the class that introduces the slot. A call through a member
/// function pointer checks it on the virtual path, and the ordinary type on
/// the non-virtual path; on a mismatch, a target that carries the marker
/// fails fast. The call itself checks nothing more.

struct Base {
  int nv(int);
  virtual int v(int);
  virtual int p(int) = 0;
  virtual ~Base();
};
struct Other { virtual int o(int); int x; };
struct Der : Other, Base {
  int v(int) override;
  int p(int) override;
};

// CHECK: define {{.*}} @_ZN4Base2nvEi({{.*}} !kcfi_type ![[#NV:]] {
int Base::nv(int x) { return x; }
// CHECK: define {{.*}} @_ZN4Base1vEi({{.*}} !kcfi_type ![[#V:]] !kcfi_vfn_type ![[#VFN:]] {
int Base::v(int x) { return x; }
/// Destructors cannot be named by a member function pointer.
// CHECK: define {{.*}} @_ZN4BaseD2Ev({{.*}} !kcfi_type ![[#]] {
Base::~Base() {}
// CHECK: define {{.*}} @_ZN3Der1vEi({{.*}} !kcfi_vfn_type ![[#VFN]] {
// CHECK: define {{.*}} @_ZThn16_N3Der1vEi.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#V]] !kcfi_vfn_type ![[#VFN]] {
int Der::v(int x) { return x + 1; }
int Der::p(int x) { return x + 2; }

// CHECK-LABEL: define {{.*}} @_Z4callP4BaseMS_FiiE(
// CHECKS:      memptr.virtual:
// CHECKS:        %memptr.virtualfn = load ptr
// CHECKS-NEXT:   [[W:%.*]] = getelementptr i8, ptr %memptr.virtualfn, i64 -16
// CHECKS-NEXT:   %kcfi.type = load i32, ptr [[W]], align 1
// CHECKS-NEXT:   [[OK:%.*]] = icmp eq i32 %kcfi.type, [[#%d,VFN_ID:]]
// CHECKS-NEXT:   br i1 [[OK]], label %kcfi.cont, label %kcfi.mismatch
// CHECKS:      kcfi.mismatch:
// CHECKS-NEXT:   [[M:%.*]] = getelementptr i8, ptr %memptr.virtualfn, i64 -12
// CHECKS-NEXT:   %kcfi.marker = load i64, ptr [[M]], align 1
// CHECKS-NEXT:   [[OURS:%.*]] = icmp eq i64 %kcfi.marker, [[#%d,PATTERN:]]
// CHECKS-NEXT:   br i1 [[OURS]], label %kcfi.fail, label %kcfi.cont
// CHECKS:      kcfi.fail:
// X64-NEXT:      call void asm sideeffect "int $$0x29", "{cx}"(i32 64)
// A64-NEXT:      call void asm sideeffect "brk #0xF003", "{w0}"(i32 64)
// CHECKS-NEXT:   unreachable
// CHECKS:      memptr.nonvirtual:
// CHECKS-NEXT:   %memptr.nonvirtualfn = inttoptr
// CHECKS-NEXT:   [[W:%.*]] = getelementptr i8, ptr %memptr.nonvirtualfn, i64 -4
// CHECKS-NEXT:   %kcfi.type{{[0-9]+}} = load i32, ptr [[W]], align 1
// CHECKS-NEXT:   {{%.*}} = icmp eq i32 %kcfi.type{{[0-9]+}}, [[#%d,NV_ID:]]
// CHECKS:      memptr.end:
// CHECK:         call noundef i32 %{{[0-9]+}}(ptr {{.*}}, i32 noundef 1){{$}}
// NOCHECKS-NOT:  kcfi.type
int call(Base *p, int (Base::*f)(int)) { return (p->*f)(1); }

/// A pure slot's stub carries the second type too.
// CHECK: define linkonce_odr hidden void @_purecall.kcfi.{{[0-9a-f]+}}() {{.*}}!kcfi_vfn_type ![[#VFN]] {

// CHECKS: ![[#NV]] = !{i32 [[#NV_ID]]}
// CHECKS: ![[#VFN]] = !{i32 [[#VFN_ID]]}
