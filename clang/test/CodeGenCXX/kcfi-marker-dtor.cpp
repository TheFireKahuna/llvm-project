// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s

/// Under the KCFI marker scheme, the complete and base destructors, and the
/// helpers that destroy arrays, carry void(void *) salted "__cxa_dtor", the
/// type the runtime calls them through. A deleting destructor, reached only
/// through the vtable, is further salted by the class that introduces its
/// slot. A virtual destructor call checks the type of its variant.

struct S { ~S(); };
struct Base { virtual ~Base(); virtual void f(); int x; };
struct A2 { virtual ~A2(); int y; };
struct Der : A2, Base { ~Der() override; };

// CHECK: define {{.*}} @_ZN1SD2Ev({{.*}} !kcfi_type ![[#DTOR:]]
// CHECK: define {{.*}} @_ZN1SD1Ev({{.*}} !kcfi_type ![[#DTOR]]
S::~S() {}

// CHECK: define {{.*}} @_ZN4BaseD2Ev({{.*}} !kcfi_type ![[#DTOR]]
// CHECK: define {{.*}} @_ZN4BaseD1Ev({{.*}} !kcfi_type ![[#DTOR]]
// CHECK: define {{.*}} @_ZN4BaseD0Ev({{.*}} !kcfi_type ![[#BASE_D0:]]
Base::~Base() {}

// CHECK: define {{.*}} @_ZN2A2D0Ev({{.*}} !kcfi_type ![[#A2_D0:]]
A2::~A2() {}

/// Der's primary chain starts at A2; its thunks occupy Base's slots.
// CHECK: define {{.*}} @_ZN3DerD2Ev({{.*}} !kcfi_type ![[#DTOR]]
// CHECK: define {{.*}} @_ZN3DerD1Ev({{.*}} !kcfi_type ![[#DTOR]]
// CHECK: define {{.*}} @_ZThn16_N3DerD1Ev.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#DTOR]]
// CHECK: define {{.*}} @_ZN3DerD0Ev({{.*}} !kcfi_type ![[#A2_D0]]
// CHECK: define {{.*}} @_ZThn16_N3DerD0Ev.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#BASE_D0]]
Der::~Der() {}

// CHECK: define internal void @__cxx_global_array_dtor({{.*}} !kcfi_type ![[#DTOR]]
S arr[2];

/// The runtime calls a destructor through a salted type.
typedef void (*SaltedDtor)(void *) __attribute__((cfi_salt("__cxa_dtor")));
// CHECK-LABEL: define {{.*}} @_Z11runtimeCallPU8cfi_saltIX10__cxa_dtorEEFvPvES_(
// CHECK:         call void %{{.*}}(ptr noundef %{{.*}}) [ "kcfi"(i32 1318596029) ]
void runtimeCall(SaltedDtor d, void *p) { d(p); }

// CHECK-LABEL: define {{.*}} @_Z12explicitDtorP4Base(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 1318596029) ]
void explicitDtor(Base *p) { p->~Base(); }

// CHECK-LABEL: define {{.*}} @_Z3delP4Base(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 2146590207) ]
void del(Base *p) { delete p; }

// CHECK-LABEL: define {{.*}} @_Z5delA2P2A2(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 880822470) ]
void delA2(A2 *p) { delete p; }

// CHECK: ![[#DTOR]] = !{i32 1318596029}
// CHECK: ![[#BASE_D0]] = !{i32 2146590207}
// CHECK: ![[#A2_D0]] = !{i32 880822470}
