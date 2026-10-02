// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,CHECKS
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++20 -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,CHECKS
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,NOCHECKS

/// Under the KCFI marker scheme, every function that can occupy a vtable slot
/// carries a second type, its function type salted "__vfn", which does not
/// depend on the class that introduces the slot. A call through a member
/// function pointer checks it 16 bytes before the entry on the virtual path,
/// and the ordinary type 4 bytes before the entry on the non-virtual path,
/// with llvm.kcfi.check. Each check hands a target outside the image to
/// Control Flow Guard, so the call carries no bundle and no guard check.

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
// CHECK:       memptr.virtual:
// CHECK:         %memptr.virtualfn = load ptr
// CHECKS-NEXT:   call void @llvm.kcfi.check(ptr %memptr.virtualfn, i32 [[#%d,VFN_ID:]], i32 16)
// CHECK-NEXT:    br label %memptr.end
// CHECK:       memptr.nonvirtual:
// CHECK-NEXT:    %memptr.nonvirtualfn = inttoptr
// CHECKS-NEXT:   call void @llvm.kcfi.check(ptr %memptr.nonvirtualfn, i32 [[#%d,NV_ID:]], i32 4)
// CHECK-NEXT:    br label %memptr.end
// CHECK:       memptr.end:
// CHECKS:        call noundef i32 %{{[0-9]+}}(ptr {{.*}}, i32 noundef 1) #[[#NOCF:]]{{$}}
// NOCHECKS:      call noundef i32 %{{[0-9]+}}(ptr {{.*}}, i32 noundef 1){{$}}
// NOCHECKS-NOT:  @llvm.kcfi.check
int call(Base *p, int (Base::*f)(int)) { return (p->*f)(1); }

/// A pure slot's stub carries the second type too.
// CHECK: define linkonce_odr hidden void @_purecall.kcfi.{{[0-9a-f]+}}() {{.*}}!kcfi_vfn_type ![[#VFN]] {

// CHECKS: attributes #[[#NOCF]] = { {{.*}}"guard_nocf"{{.*}} }

// CHECKS-DAG: ![[#VFN]] = !{i32 [[#VFN_ID]]}
// CHECKS-DAG: ![[#NV]] = !{i32 [[#NV_ID]]}
