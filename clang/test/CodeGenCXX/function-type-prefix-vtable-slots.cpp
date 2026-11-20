// DEFINE: %{cc} = %clang_cc1 -std=c++17 -fsanitize-cfi-icall-generalize-pointers -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fsanitize=kcfi | FileCheck %s --check-prefixes=CHECK,CHECKS
// RUN: %{cc} -triple aarch64-pc-windows-ntposix -fsanitize=kcfi | FileCheck %s --check-prefixes=CHECK,CHECKS
// RUN: %{cc} -triple x86_64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,NOCHECKS
// RUN: %{cc} -triple x86_64-unknown-linux-gnu -fsanitize=kcfi | FileCheck %s --check-prefix=UPSTREAM

/// Under function type prefixes, a function that can fill a vtable slot
/// carries the KCFI type of the slot, its signature salted by the class that
/// introduces the slot, which a virtual call checks, and a second type, the
/// signature salted "__vfn", which a call through a member function pointer
/// checks. A destructor carries void(void *) salted "__cxa_dtor", the type the
/// runtime calls one through, and a deleting destructor that salted by the
/// class besides. A thunk carries the type of the slot it fills and is named
/// after it, and a pure or deleted slot holds a stub of its type. The types
/// are there whether or not the unit checks calls.

struct A {
  virtual ~A();
  virtual int f(int);
  virtual void pure() = 0;
  virtual void gone() = delete;
  int nv(int);
};
struct B {
  virtual int g(int);
  long pad;
};
struct C : A, B {
  ~C() override;
  int f(int) override;
  int g(int) override;
};

// CHECK: @_ZTV1A = {{.*}} ptr @_ZN1A1fEi, ptr @[[PURE:_purecall|__cxa_pure_virtual]].kcfi.94ba7fdb, ptr @__cxa_deleted_virtual.kcfi.94ba7fdb]
// CHECK: @_ZTV1C = {{.*}} ptr @_ZThn8_N1C1gEi.kcfi.4dddf17a]

// CHECK: define dso_local void @_ZN1AD2Ev({{.*}} !kcfi_type ![[#DTOR:]] {
// CHECK: define dso_local void @_ZN1AD1Ev({{.*}} !kcfi_type ![[#DTOR]] {
// CHECK: define dso_local void @_ZN1AD0Ev({{.*}} !kcfi_type ![[#A_D0:]] {
A::~A() {}
// CHECK: define dso_local noundef i32 @_ZN1A1fEi({{.*}} !kcfi_type ![[#A_F:]] !kcfi_vfn_type ![[#VFN:]] {
int A::f(int x) { return x; }
/// A member function that is not virtual carries its plain type.
// CHECK: define dso_local noundef i32 @_ZN1A2nvEi({{.*}} !kcfi_type ![[#PLAIN:]] {
int A::nv(int x) { return x; }

// CHECK: define dso_local void @_ZN1CD1Ev({{.*}} !kcfi_type ![[#DTOR]] {
// CHECK: define dso_local void @_ZN1CD0Ev({{.*}} !kcfi_type ![[#A_D0]] {
C::~C() {}
/// An override carries the type of the slot that its base class introduced.
// CHECK: define dso_local noundef i32 @_ZN1C1fEi({{.*}} !kcfi_type ![[#A_F]] !kcfi_vfn_type ![[#VFN]] {
int C::f(int x) { return x + 1; }
/// C::g fills a slot of its own in C's vtable, and its thunk B's slot.
// CHECK: define dso_local noundef i32 @_ZN1C1gEi({{.*}} !kcfi_type ![[#C_G:]] !kcfi_vfn_type ![[#VFN]] {
// CHECK: define dso_local noundef i32 @_ZThn8_N1C1gEi.kcfi.4dddf17a({{.*}} !kcfi_type ![[#B_G:]] !kcfi_vfn_type ![[#VFN]] {
int C::g(int x) { return x + 2; }

// CHECK-LABEL: define dso_local noundef i32 @_Z5vcallP1A(
// CHECKS:   call noundef i32 %{{.*}}(ptr {{.*}}, i32 noundef 1) [ "kcfi"(i32 -45758786) ]
// NOCHECKS: call noundef i32 %{{.*}}(ptr {{.*}}, i32 noundef 1){{$}}
int vcall(A *a) { return a->f(1); }

/// A virtual destructor call checks the type of the variant it calls.
// CHECK-LABEL: define dso_local void @_Z4vdelP1A(
// CHECKS: call void %{{.*}} [ "kcfi"(i32 1380494204) ]
void vdel(A *a) { delete a; }
// CHECK-LABEL: define dso_local void @_Z5vdtorP1A(
// CHECKS: call void %{{.*}} [ "kcfi"(i32 1318596029) ]
void vdtor(A *a) { a->~A(); }

/// A call through a member function pointer checks the second type on the
/// virtual path and the plain type on the other, and nothing more at the call.
// CHECK-LABEL: define dso_local noundef i32 @_Z5mcallP1AMS_FiiE(
// CHECKS:      call void @llvm.kcfi.check(ptr %memptr.virtualfn, i32 -91764089, i32 16)
// CHECKS:      call void @llvm.kcfi.check(ptr %memptr.nonvirtualfn, i32 -1526708030, i32 4)
// CHECKS:      call noundef i32 %{{.*}}(ptr {{.*}}, i32 noundef 2) #[[NOCF:[0-9]+]]{{$}}
// NOCHECKS-NOT: @llvm.kcfi.check
int mcall(A *a, int (A::*p)(int)) { return (a->*p)(2); }

/// Clang registers a destructor, or a helper that destroys an array, through
/// the runtimes' sibling entry points, which call it through the type it
/// carries.
struct S { ~S(); };
// CHECK: call i32 @__llvm_kcfi_cxa_atexit(ptr @_ZN1SD1Ev, ptr @s,
S s;
// CHECK: call i32 @__llvm_kcfi_cxa_atexit(ptr @__cxx_global_array_dtor, ptr null,
// CHECK: define internal void @__cxx_global_array_dtor(ptr noundef %0) {{.*}}!kcfi_type ![[#DTOR]] {
S arr[2];
// CHECK: call i32 @__llvm_kcfi_cxa_thread_atexit(ptr @_ZN1SD1Ev, ptr @t,
thread_local S t;

// CHECK: define linkonce_odr hidden void @[[PURE]].kcfi.94ba7fdb() unnamed_addr {{.*}}comdat !kcfi_type ![[#A_PURE:]] !kcfi_vfn_type ![[#VFN_VOID:]] {
// CHECK-NEXT: entry:
// CHECK-NEXT:   call void @[[PURE]]() #[[NORETURN:[0-9]+]]
// CHECK-NEXT:   unreachable
// CHECK: define linkonce_odr hidden void @__cxa_deleted_virtual.kcfi.94ba7fdb() unnamed_addr {{.*}}comdat !kcfi_type ![[#A_PURE]] !kcfi_vfn_type ![[#VFN_VOID]] {

// CHECK:  attributes #[[NORETURN]] = { noreturn nounwind }
// CHECKS: attributes #[[NOCF]] = { "guard_nocf" }

// CHECK: ![[#DTOR]] = !{i32 1318596029}
// CHECK: ![[#A_D0]] = !{i32 1380494204}
// CHECK: ![[#A_F]] = !{i32 -45758786}
// CHECK: ![[#VFN]] = !{i32 -91764089}
// CHECK: ![[#PLAIN]] = !{i32 -1526708030}
// CHECK: ![[#C_G]] = !{i32 -1177288725}
// CHECK: ![[#B_G]] = !{i32 1306390906}
// CHECK: ![[#A_PURE]] = !{i32 -1799716901}
// CHECK: ![[#VFN_VOID]] = !{i32 -1621490657}

/// Elsewhere a virtual function carries its plain type, no call through a
/// vtable is checked, and a destructor is registered as before.
// UPSTREAM: @_ZTV1A = {{.*}} ptr @__cxa_pure_virtual, ptr @__cxa_deleted_virtual]
// UPSTREAM: define dso_local noundef i32 @_ZN1A1fEi({{.*}} !kcfi_type ![[#PLAIN:]] {
// UPSTREAM: define dso_local noundef i32 @_ZN1A2nvEi({{.*}} !kcfi_type ![[#PLAIN]] {
// UPSTREAM: define dso_local noundef i32 @_ZThn8_N1C1gEi(
// UPSTREAM-NOT: @llvm.kcfi.check
// UPSTREAM: call i32 @__cxa_atexit(ptr @_ZN1SD1Ev,
// UPSTREAM-NOT: kcfi_vfn_type
