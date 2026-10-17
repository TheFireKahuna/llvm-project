// RUN: %clang_cc1 -flto -triple x86_64-unknown-windows-itanium -fwhole-program-vtables -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,DEFAULT
// RUN: %clang_cc1 -flto -triple aarch64-unknown-windows-itanium -fwhole-program-vtables -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,DEFAULT
// RUN: %clang_cc1 -flto -triple x86_64-pc-windows-ntposix -fwhole-program-vtables -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,DEFAULT
// RUN: %clang_cc1 -flto -triple x86_64-unknown-windows-itanium -fwhole-program-vtables -fvisibility=hidden -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,HIDDEN
// RUN: %clang_cc1 -flto -triple x86_64-pc-windows-gnu -fwhole-program-vtables -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,MINGW

/// On Windows Itanium and NT-POSIX, visibility is the boundary of an image,
/// so a class has hidden LTO visibility only when it has hidden visibility,
/// as on ELF. Other COFF targets give every class without dllimport or
/// dllexport hidden LTO visibility.

struct C1 {
  virtual void f();
};

struct __attribute__((visibility("hidden"))) C2 {
  virtual void f();
};

struct __attribute__((visibility("default"))) C3 {
  virtual void f();
};

namespace {
struct C4 {
  virtual void f() {}
};
} // namespace

// CHECK-LABEL: define {{.*}} @_Z2f1P2C1(
// DEFAULT:       call i1 @llvm.public.type.test(ptr %{{.*}}, metadata !"_ZTS2C1")
// HIDDEN:        call i1 @llvm.type.test(ptr %{{.*}}, metadata !"_ZTS2C1")
// MINGW:         call i1 @llvm.type.test(ptr %{{.*}}, metadata !"_ZTS2C1")
void f1(C1 *p) { p->f(); }

// CHECK-LABEL: define {{.*}} @_Z2f2P2C2(
// CHECK:         call i1 @llvm.type.test(ptr %{{.*}}, metadata !"_ZTS2C2")
void f2(C2 *p) { p->f(); }

// CHECK-LABEL: define {{.*}} @_Z2f3P2C3(
// DEFAULT:       call i1 @llvm.public.type.test(ptr %{{.*}}, metadata !"_ZTS2C3")
// HIDDEN:        call i1 @llvm.public.type.test(ptr %{{.*}}, metadata !"_ZTS2C3")
// MINGW:         call i1 @llvm.type.test(ptr %{{.*}}, metadata !"_ZTS2C3")
void f3(C3 *p) { p->f(); }

// CHECK-LABEL: define {{.*}} @_Z2f4v(
// CHECK:         call i1 @llvm.type.test(ptr %{{.*}}, metadata ![[#]])
void f4() {
  C4 c;
  C4 *p = &c;
  p->f();
}
