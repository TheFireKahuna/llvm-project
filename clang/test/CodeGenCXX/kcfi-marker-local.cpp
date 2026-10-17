// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s

/// A virtual call checks a KCFI type salted by the class that introduces the
/// slot. When that class has internal linkage, every function that can occupy
/// the slot is in this translation unit, and the call is marked kcfi_local. A
/// destructor that does not delete has a type no class salts, so its call is
/// not marked.

namespace {
struct L {
  virtual void f() {}
  virtual ~L() {}
};
} // namespace

struct E {
  virtual void f();
  virtual ~E();
};

void *opaque();

// CHECK-LABEL: define dso_local void @_Z5callLv(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 {{-?[0-9]+}}) ], !kcfi_local ![[#LOCAL:]]
void callL() { static_cast<L *>(opaque())->f(); }

// CHECK-LABEL: define dso_local void @_Z5callEP1E(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 {{-?[0-9]+}}) ]{{$}}
void callE(E *p) { p->f(); }

// CHECK-LABEL: define dso_local void @_Z7deleteLv(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 {{-?[0-9]+}}) ], !kcfi_local ![[#LOCAL]]
void deleteL() { delete static_cast<L *>(opaque()); }

// CHECK-LABEL: define dso_local void @_Z8destroyLv(
// CHECK:         call void %{{.*}} [ "kcfi"(i32 {{-?[0-9]+}}) ]{{$}}
void destroyL() { static_cast<L *>(opaque())->~L(); }

// CHECK: ![[#LOCAL]] = !{}
