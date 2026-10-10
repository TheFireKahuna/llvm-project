// RUN: %clang_cc1 -std=c17 -Wno-deprecated-non-prototype -triple x86_64-unknown-linux-gnu -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s

/// A function without a prototype has the type of its parameters after the
/// default argument promotions, and a call through a pointer without a
/// prototype checks the type of its promoted arguments, so that every call
/// that C17 defines passes the check and every call it leaves undefined fails.

/// void(void)
// CHECK: define{{.*}} void @f(){{.*}} !kcfi_type ![[#VOID:]]
void f() {}

/// void(int, double)
// CHECK: define{{.*}} void @g({{.*}} !kcfi_type ![[#INT_DOUBLE:]]
void g(int a, double d) {}

/// A definition without a prototype promotes short to int: void(int, int).
// CHECK: define{{.*}} void @h({{.*}} !kcfi_type ![[#INT_INT:]]
void h(a, s) int a; short s; {}

/// The promoted argument types never match these.
// CHECK: define{{.*}} void @k({{.*}} !kcfi_type ![[#SHORT:]]
void k(short s) {}
// CHECK: define{{.*}} void @fl({{.*}} !kcfi_type ![[#FLOAT:]]
void fl(float x) {}
// CHECK: define{{.*}} void @v({{.*}} !kcfi_type ![[#VARIADIC:]]
void v(int a, ...) {}

/// f is valid through a pointer to void(void).
// CHECK-LABEL: define{{.*}} void @call_void(
// CHECK: call void %{{.*}}() [ "kcfi"(i32 -1522505972) ]
void call_void(void (*p)(void)) { p(); }

/// g is valid through a pointer without a prototype, the float promoted.
// CHECK-LABEL: define{{.*}} void @call_g(
// CHECK: call {{.*}} [ "kcfi"(i32 1416889344) ]
void call_g(void (*p)()) { p(1, 2.0f); }

/// So is h, the short promoted.
// CHECK-LABEL: define{{.*}} void @call_h(
// CHECK: call {{.*}} [ "kcfi"(i32 -318401964) ]
void call_h(void (*p)()) {
  short s = 1;
  p(1, s);
}

/// void(int) matches neither g, a wrong argument count, nor v, a variadic
/// function.
// CHECK-LABEL: define{{.*}} void @call_int(
// CHECK: call {{.*}} [ "kcfi"(i32 27004076) ]
void call_int(void (*p)()) { p(1); }

/// A short argument promotes to int, void(int), and does not match k.
// CHECK-LABEL: define{{.*}} void @call_short(
// CHECK: call {{.*}} [ "kcfi"(i32 27004076) ]
void call_short(void (*p)()) {
  short s = 1;
  p(s);
}

/// A float argument promotes to double, void(double), and does not match fl.
// CHECK-LABEL: define{{.*}} void @call_float(
// CHECK: call {{.*}} [ "kcfi"(i32 -154794606) ]
void call_float(void (*p)()) { p(1.0f); }

/// The rebuilt type keeps the calling convention.
// CHECK: define{{.*}} void @m(){{.*}} !kcfi_type ![[#MS_VOID:]]
__attribute__((ms_abi)) void m() {}
// CHECK-LABEL: define{{.*}} void @call_ms(
// CHECK: call win64cc void %{{.*}}() [ "kcfi"(i32 [[#%d,MS_VOID_ID:]]) ]
void call_ms(__attribute__((ms_abi)) void (*p)(void)) { p(); }

/// A declaration whose address is taken before its definition takes the
/// definition's type.
void later();
// CHECK-LABEL: define{{.*}} void @take_later(
// CHECK: store ptr @later
void take_later(void (**p)(void)) { *p = later; }
// CHECK: define{{.*}} void @later(){{.*}} !kcfi_type ![[#VOID]]
void later() {}

// CHECK: ![[#VOID]] = !{i32 -1522505972}
// CHECK: ![[#INT_DOUBLE]] = !{i32 1416889344}
// CHECK: ![[#INT_INT]] = !{i32 -318401964}
// CHECK: ![[#SHORT]] = !{i32 469935645}
// CHECK: ![[#FLOAT]] = !{i32 1483352686}
// CHECK: ![[#VARIADIC]] = !{i32 -1902991328}
// CHECK: ![[#MS_VOID]] = !{i32 [[#MS_VOID_ID]]}
