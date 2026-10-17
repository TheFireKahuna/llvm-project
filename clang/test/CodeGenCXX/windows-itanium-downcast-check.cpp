// RUN: %clang_cc1 %s -triple x86_64-unknown-windows-itanium -O1 \
// RUN:   -disable-llvm-passes -emit-llvm -fsanitize=cfi-derived-cast \
// RUN:   -fsanitize-trap=cfi-derived-cast -o - \
// RUN:   | FileCheck %s --check-prefixes=CHECK,TRAP
// RUN: %clang_cc1 %s -triple aarch64-pc-windows-ntposix -O1 \
// RUN:   -disable-llvm-passes -emit-llvm -fsanitize=cfi-derived-cast \
// RUN:   -fsanitize-trap=cfi-derived-cast -o - \
// RUN:   | FileCheck %s --check-prefixes=CHECK,TRAP
// RUN: %clang_cc1 %s -triple x86_64-unknown-windows-itanium -O1 \
// RUN:   -disable-llvm-passes -emit-llvm -fsanitize=cfi-derived-cast \
// RUN:   -fsanitize-recover=cfi-derived-cast -o - \
// RUN:   | FileCheck %s --check-prefixes=CHECK,REPORT
// RUN: %clang_cc1 %s -triple x86_64-unknown-windows-itanium -O1 \
// RUN:   -disable-llvm-passes -emit-llvm -fsanitize=cfi-derived-cast \
// RUN:   -fsanitize-trap=cfi-derived-cast -fno-rtti -o - \
// RUN:   | FileCheck %s --check-prefix=NORTTI

// On Windows Itanium and NT-POSIX a static_cast from a polymorphic base to a
// derived class is checked without LTO: a class final in effect by the exact
// dynamic_cast, any other by the run of vtables the linker lays out for the
// class and the classes derived from it through primary bases. A miss is
// confirmed by __dynamic_cast, and a failure fails fast with code 65, or,
// without trapping, calls a weak routine whose default reports the failure. A
// class another image may own is first tested against the owner's run, which
// the linker binds as three words: its start, its end, and its start plus the
// slot size less one.

struct A { virtual ~A(); };
struct B : A { virtual void f(); };
struct L final : B { void f() override; };
struct P : private A { static P *down(A *a); };

// The routine's default reports through the slot the loader binds.
// REPORT: @__castguard_check_failure_os_handled_fptr = linkonce_odr hidden constant ptr null, section ".00cfg", comdat, align 8
// REPORT: @__llvm_cast_check_fail = weak hidden alias void (ptr, ptr), ptr @__llvm_cast_check_report

// CHECK-LABEL: define {{.*}} @_Z7to_baseP1A(
// CHECK: %[[VT:vtable[0-9]*]] = load ptr, ptr
// CHECK: icmp uge ptr %[[VT]], @"__llvm_vtables$1B"
// CHECK: icmp ult ptr %[[VT]], @"__llvm_vtables_end$1B"
// CHECK: %[[OFF:.*]] = sub i64 %{{.*}}, ptrtoint (ptr @"__llvm_vtables$1B" to i64)
// CHECK: %[[MASK:.*]] = load i32, ptr @"__llvm_vtables_mask$1B"
// CHECK: %[[LOW:.*]] = trunc i64 %[[OFF]] to i32
// CHECK: and i32 %[[LOW]], %[[MASK]]
// CHECK: cast.check.owner:
// CHECK: %[[OSTART:.*]] = load ptr, ptr @"__llvm_vtables_owner$1B", align 8
// CHECK: %[[OS:.*]] = ptrtoint ptr %[[OSTART]] to i64
// CHECK: %[[OOFF:.*]] = sub i64 %{{.*}}, %[[OS]]
// CHECK: load ptr, ptr getelementptr inbounds nuw (i8, ptr @"__llvm_vtables_owner$1B", i64 8)
// CHECK: icmp ult i64 %[[OOFF]],
// CHECK: load ptr, ptr getelementptr inbounds nuw (i8, ptr @"__llvm_vtables_owner$1B", i64 16)
// CHECK: and i64 %[[OOFF]],
// CHECK: cast.check.miss:
// CHECK: %[[FOUND:.*]] = call ptr @__dynamic_cast(ptr %{{.*}}, ptr @_ZTI1A, ptr @_ZTI1B, i64 0)
// CHECK: %[[OK:.*]] = icmp eq ptr %[[FOUND]], %{{.*}}
// TRAP-NOT: load ptr
// TRAP: br i1 %[[OK]], label %{{.*}}, label %[[TRAPBB:[a-z0-9.]+]],
// TRAP: [[TRAPBB]]:
// TRAP: call void @llvm.ubsantrap(i8 65)
// REPORT: br i1 %[[OK]], label %cast.check.cont, label %cast.check.fail
// REPORT: cast.check.fail:
// REPORT: call void @__llvm_cast_check_fail(ptr %{{.*}}, ptr @_ZTI1B)
// REPORT: br label %cast.check.cont
// CHECK: ret ptr
// REPORT: define linkonce_odr hidden void @__llvm_cast_check_report(ptr %0, ptr %1) {{.*}}comdat
// REPORT: %[[R:.*]] = load volatile ptr, ptr @__castguard_check_failure_os_handled_fptr
// REPORT: call win64cc void %[[R]](ptr %0) #[[NOCF:[0-9]+]]
B *to_base(A *a) { return static_cast<B *>(a); }

// A class of hidden visibility is the image's own and has no owner's run.
// CHECK-LABEL: define {{.*}} @_Z9to_hiddenP1A(
// CHECK: @"__llvm_vtables$1H"
// CHECK-NOT: __llvm_vtables_owner
// CHECK: ret ptr
struct __attribute__((visibility("hidden"))) H : A { virtual void h(); };
H *to_hidden(A *a) { return static_cast<H *>(a); }

// A reference cast takes the same check, without the null test.
// CHECK-LABEL: define {{.*}} @_Z6to_refR1A(
// CHECK-NOT: icmp eq ptr %{{.*}}, null
// CHECK: @"__llvm_vtables$1B"
// CHECK: @__dynamic_cast
B &to_ref(A &a) { return static_cast<B &>(a); }

// A final class takes the exact cast.
// CHECK-LABEL: define {{.*}} @_Z7to_leafP1A(
// CHECK-NOT: __llvm_vtables
// CHECK: icmp eq ptr %{{.*}}, getelementptr inbounds {{.*}}@_ZTV1L
// CHECK: cast.check.miss:
// CHECK: call ptr @__dynamic_cast(ptr %{{.*}}, ptr @_ZTI1A, ptr @_ZTI1L,
L *to_leaf(A *a) { return static_cast<L *>(a); }

// A downcast through a private base is not checked: __dynamic_cast finds
// only public ones.
// CHECK-LABEL: define {{.*}} @_ZN1P4downEP1A(
// CHECK-NOT: __dynamic_cast
// CHECK: ret ptr
P *P::down(A *a) { return static_cast<P *>(a); }

// An opted-out function is not checked.
// CHECK-LABEL: define {{.*}} @_Z9unchecked
// CHECK-NOT: __dynamic_cast
// CHECK: ret ptr
__attribute__((no_sanitize("cfi-derived-cast"))) B *unchecked(A *a) {
  return static_cast<B *>(a);
}

// TRAP-NOT: @__llvm_cast_check_fail
// REPORT: attributes #[[NOCF]] = { "guard_nocf" }

// Without RTTI a miss could not be confirmed, so nothing is checked.
// NORTTI-NOT: __llvm_vtables
// NORTTI-NOT: ubsantrap
