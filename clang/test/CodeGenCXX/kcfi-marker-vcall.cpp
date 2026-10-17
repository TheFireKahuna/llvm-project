// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,CHECKS
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,CHECKS
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefixes=CHECK,NOCHECKS
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefix=UPSTREAM

/// Under the KCFI marker scheme, a virtual call checks the type of its vtable
/// slot: the function type salted by the class that introduces the slot. Every
/// function that can occupy the slot carries that type: an overrider in the
/// slot, and a thunk for it in another. Prefixes carry these types whether or
/// not calls are checked; without the scheme, virtual calls are not checked.

struct A { virtual int fa(int); int a; };
struct B { virtual int fb(int); virtual B *clone(); int b; };
struct D : A, B {
  int fa(int) override;
  int fb(int) override;
  D *clone() override;
};
struct V { virtual int fv(int); };
struct W : virtual V { int fv(int) override; };
struct V1 { virtual int fm(int); int v1; };
struct V2 { virtual int fm(int); int v2; };
struct X : A, virtual V1, virtual V2 { int fm(int) override; };

namespace {
struct Local { virtual int fl(int); };
int Local::fl(int x) { return x; }
}

// CHECK: define {{.*}} @_ZN1A2faEi({{.*}} !kcfi_type ![[#A_FA:]]
int A::fa(int x) { return x; }
// CHECK: define {{.*}} @_ZN1B2fbEi({{.*}} !kcfi_type ![[#B_FB:]]
int B::fb(int x) { return x; }
// CHECK: define {{.*}} @_ZN1B5cloneEv({{.*}} !kcfi_type ![[#B_CLONE:]]
B *B::clone() { return this; }

/// D::fa occupies A's slot; D::fb and D::clone introduce slots of their own
/// in D's primary vtable, and their thunks occupy B's slots.
// CHECK: define {{.*}} @_ZN1D2faEi({{.*}} !kcfi_type ![[#A_FA]]
int D::fa(int x) { return x + 1; }
// CHECK: define {{.*}} @_ZN1D2fbEi({{.*}} !kcfi_type ![[#D_FB:]]
// CHECK: define {{.*}} @_ZThn16_N1D2fbEi.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#B_FB]]
int D::fb(int x) { return x + 2; }
// CHECK: define {{.*}} @_ZN1D5cloneEv({{.*}} !kcfi_type ![[#D_CLONE:]]
// CHECK: define {{.*}} @_ZTchn16_h16_N1D5cloneEv.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#B_CLONE]]
D *D::clone() { return this; }

/// A virtual-base thunk occupies V's slot.
// CHECK: define {{.*}} @_ZN1W2fvEi({{.*}} !kcfi_type ![[#V_FV:]]
// CHECK: define {{.*}} @_ZTv0_n24_N1W2fvEi.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#V_FV]]
int W::fv(int x) { return x; }

/// A thunk that occupies slots of different types, here those that V1 and V2
/// introduce, has a copy for each, named after the type.
// CHECK: define {{.*}} @_ZN1X2fmEi(
// CHECK: define {{.*}} @_ZTv0_n24_N1X2fmEi.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#V1_FM:]]
// CHECK: define {{.*}} @_ZTv0_n24_N1X2fmEi.kcfi.{{[0-9a-f]+}}({{.*}} !kcfi_type ![[#V2_FM:]]
int X::fm(int x) { return x; }

// CHECK-LABEL: define {{.*}} @_Z5callAP1A(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,A_FA_ID:]]) ]
// NOCHECKS-NOT:  "kcfi"
// UPSTREAM-LABEL: define {{.*}} @_Z5callAP1A(
// UPSTREAM-NOT:   "kcfi"
// UPSTREAM:       ret i32
int callA(A *p) { return p->fa(1); }
// CHECK-LABEL: define {{.*}} @_Z5callBP1B(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,B_FB_ID:]]) ]
int callB(B *p) { return p->fb(1); }
// CHECK-LABEL: define {{.*}} @_Z5callCP1B(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,B_CLONE_ID:]]) ]
B *callC(B *p) { return p->clone(); }
// CHECK-LABEL: define {{.*}} @_Z5callDP1D(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,D_FB_ID:]]) ]
int callD(D *p) { return p->fb(1); }
// CHECK-LABEL: define {{.*}} @_Z5callVP1V(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,V_FV_ID:]]) ]
int callV(V *p) { return p->fv(1); }
// CHECK-LABEL: define {{.*}} @_Z6callV1P2V1(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,V1_FM_ID:]]) ]
int callV1(V1 *p) { return p->fm(1); }
// CHECK-LABEL: define {{.*}} @_Z6callV2P2V2(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,V2_FM_ID:]]) ]
int callV2(V2 *p) { return p->fm(1); }

/// A class with internal linkage is checked the same way.
// CHECK-LABEL: define {{.*}} @_Z5callLv(
// CHECKS:        call {{.*}} [ "kcfi"(i32 [[#%d,LOCAL_ID:]]) ]
int callL() {
  Local *p = new Local;
  return p->fl(1);
}
// CHECK: define internal {{.*}} @_ZN12_GLOBAL__N_15Local2flEi({{.*}} !kcfi_type ![[#LOCAL:]]

// CHECKS: ![[#A_FA]] = !{i32 [[#A_FA_ID]]}
// CHECKS: ![[#B_FB]] = !{i32 [[#B_FB_ID]]}
// CHECKS: ![[#B_CLONE]] = !{i32 [[#B_CLONE_ID]]}
// CHECKS: ![[#D_FB]] = !{i32 [[#D_FB_ID]]}
// CHECKS: ![[#V_FV]] = !{i32 [[#V_FV_ID]]}
// CHECKS: ![[#V1_FM]] = !{i32 [[#V1_FM_ID]]}
// CHECKS: ![[#V2_FM]] = !{i32 [[#V2_FM_ID]]}
// CHECKS: ![[#LOCAL]] = !{i32 [[#LOCAL_ID]]}
