// DEFINE: %{cc} = %clang_cc1 -O2 -fno-function-type-prefix -emit-llvm -o - %s
// DEFINE: %{preempt} = FileCheck %s --check-prefix=PREEMPT --implicit-check-not=dso_local
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fsemantic-interposition | %{preempt}
// RUN: %{cc} -triple aarch64-unknown-windows-itanium -fsemantic-interposition | %{preempt}
// RUN: %{cc} -triple x86_64-pc-windows-ntposix -fsemantic-interposition | %{preempt}
// RUN: %{cc} -triple x86_64-unknown-windows-itanium \
// RUN:   | FileCheck %s --check-prefix=LOCAL

// Under -fsemantic-interposition another image's definition may preempt a
// function with default visibility, as on ELF, so it is not dso_local: its
// unit neither inlines it nor binds a call to it. So may a replacement of
// operator new, which a DLL offers, and a function declared dllexport. A
// hidden function stays local, and so does an inline one, every definition of
// which is the same. Without the option every definition is local.

typedef __SIZE_TYPE__ size_t;
static char pool[8];
void *operator new(size_t n) { return pool + n; }
int f() { return 1; }
__attribute__((dllexport)) int e() { return 2; }
__attribute__((visibility("hidden"))) int h() { return 3; }
inline int i() { return 4; }

// PREEMPT-LABEL: define {{.*}}ptr @_Znw{{[my]}}(
// PREEMPT-LABEL: define {{.*}}i32 @_Z1fv(
// PREEMPT-LABEL: define dllexport {{.*}}i32 @_Z1ev(
// PREEMPT-LABEL: define hidden {{.*}}i32 @_Z1hv(
// PREEMPT-LABEL: define {{.*}}i32 @_Z3usev(
// PREEMPT:         call {{.*}}i32 @_Z1fv()
// PREEMPT-NEXT:    call {{.*}}i32 @_Z1ev()
// PREEMPT-NEXT:    add i32 %{{.*}}, 7
// PREEMPT-LABEL: define {{.*}}ptr @_Z5allocv(
// PREEMPT:         call {{.*}}ptr @_Znw{{[my]}}(i64 {{.*}}1)
// LOCAL-LABEL:   define dso_local {{.*}}ptr @_Znw{{[my]}}(
// LOCAL-LABEL:   define dso_local {{.*}}i32 @_Z3usev(
// LOCAL-NEXT:    entry:
// LOCAL-NEXT:      ret i32 10
// LOCAL-LABEL:   define dso_local {{.*}}ptr @_Z5allocv(
// LOCAL-NOT:       call
// LOCAL:           ret
int use() { return f() + e() + h() + i(); }
void *alloc() { return ::operator new(1); }
