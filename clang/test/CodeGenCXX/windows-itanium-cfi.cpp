// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:     -flto -flto-unit -fsanitize=kcfi,cfi-vcall,cfi-nvcall,cfi-derived-cast,cfi-icall,cfi-mfcall \
// RUN:     -fsanitize-trap=cfi-vcall,cfi-nvcall,cfi-derived-cast,cfi-icall,cfi-mfcall \
// RUN:     -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:     -flto -flto-unit -fsanitize=kcfi,cfi-vcall,cfi-nvcall,cfi-derived-cast,cfi-icall,cfi-mfcall \
// RUN:     -fsanitize-trap=cfi-vcall,cfi-nvcall,cfi-derived-cast,cfi-icall,cfi-mfcall \
// RUN:     -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s \
// RUN:     -flto -flto-unit -fsanitize=cfi-vcall,cfi-nvcall -fsanitize-trap=cfi-vcall,cfi-nvcall \
// RUN:   | FileCheck --check-prefix=ELF %s

// On Windows Itanium a failed CFI check fails fast: with code 64 for a check
// on an indirect transfer, as KCFI's, and 65 for an object of the wrong class
// without one. A virtual call whose vtable CFI vetted calls the function it
// read with neither a KCFI check nor Control Flow Guard's.

struct __attribute__((visibility("hidden"))) H {
  virtual void f();
  void g();
};
struct __attribute__((visibility("hidden"))) D : H {
  void f() override;
};
struct P {
  virtual void f();
};

// CHECK-LABEL: define {{.*}}@_Z5vcallP1H(
// CHECK:       call i1 @llvm.type.test(ptr {{.*}}, metadata !"_ZTS1H")
// CHECK:       call void @llvm.ubsantrap(i8 64)
// CHECK:       call void %{{[0-9a-z]+}}(ptr {{.*}}) [[VETTED:#[0-9]+]]{{$}}
// ELF-LABEL:   define {{.*}}@_Z5vcallP1H(
// ELF:         call void @llvm.ubsantrap(i8 2)
void vcall(H *h) { h->f(); }

// A class the LTO unit need not see whole keeps KCFI's check.
// CHECK-LABEL: define {{.*}}@_Z11vcallPublicP1P(
// CHECK-NOT:   llvm.type.test
// CHECK:       call void %{{[0-9a-z]+}}(ptr {{.*}}) [ "kcfi"(i32 {{-?[0-9]+}}) ]
void vcallPublic(P *p) { p->f(); }

// CHECK-LABEL: define {{.*}}@_Z6nvcallP1H(
// CHECK:       call void @llvm.ubsantrap(i8 65)
// ELF-LABEL:   define {{.*}}@_Z6nvcallP1H(
// ELF:         call void @llvm.ubsantrap(i8 2)
void nvcall(H *h) { h->g(); }

// CHECK-LABEL: define {{.*}}@_Z4castP1H(
// CHECK:       call void @llvm.ubsantrap(i8 65)
D *cast(H *h) { return static_cast<D *>(h); }

// CHECK-LABEL: define {{.*}}@_Z5icallPFvvE(
// CHECK:       call void @llvm.ubsantrap(i8 64)
void icall(void (*fp)()) { fp(); }

// CHECK-LABEL: define {{.*}}@_Z6mfcallP1HMS_FvvE(
// CHECK:       call void @llvm.ubsantrap(i8 64)
void mfcall(H *h, void (H::*mf)()) { (h->*mf)(); }

// CHECK: attributes [[VETTED]] = { {{.*}}"guard_nocf"{{.*}} }
