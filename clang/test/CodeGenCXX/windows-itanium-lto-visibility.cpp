// DEFINE: %{cc} = %clang_cc1 -flto -fwhole-program-vtables -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,PUBLIC
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,PUBLIC
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fvisibility=hidden | FileCheck %s --check-prefixes=CHECK,HIDDEN
// RUN: %{cc} -triple x86_64-w64-mingw32 | FileCheck %s --check-prefixes=CHECK,HIDDEN

/// On Windows Itanium and NT-POSIX visibility is an image's boundary, so a
/// class has hidden LTO visibility only where its visibility is hidden, as on
/// ELF: another image may derive from any other class. The other COFF targets
/// take every class without a DLL attribute to be in the linkage unit.

struct Plain {
  virtual void f();
};
struct __attribute__((visibility("hidden"))) Hidden {
  virtual void f();
};

// CHECK-LABEL: define {{.*}} @_Z5plainP5Plain(
// PUBLIC:        call i1 @llvm.public.type.test(ptr %{{.*}}, metadata !"_ZTS5Plain")
// HIDDEN:        call i1 @llvm.type.test(ptr %{{.*}}, metadata !"_ZTS5Plain")
void plain(Plain *p) { p->f(); }

// CHECK-LABEL: define {{.*}} @_Z6hiddenP6Hidden(
// CHECK:         call i1 @llvm.type.test(ptr %{{.*}}, metadata !"_ZTS6Hidden")
void hidden(Hidden *p) { p->f(); }
