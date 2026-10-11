// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 -O1 \
// RUN:   -mconstructor-aliases -mdefault-visibility-export-mapping=explicit \
// RUN:   -disable-llvm-passes -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++17 -O1 \
// RUN:   -mconstructor-aliases -mdefault-visibility-export-mapping=explicit \
// RUN:   -disable-llvm-passes -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -O1 \
// RUN:   -mconstructor-aliases -disable-llvm-passes -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefix=ELF %s

/// A base destructor equivalent to that of a base class that another image
/// defines is emitted, rather than replaced by the imported one, so that the
/// class's vtable entry is not an import of its own: a forwarder to the base
/// destructor, which the linker replaces by it where the image defines it.
/// One equivalent to a base destructor defined here is still replaced by it.

#define API __attribute__((visibility("default")))

struct API Imported {
  virtual ~Imported();
};
struct API D1 : Imported {};

struct API Local {
  virtual ~Local();
};
Local::~Local() {}
struct API D2 : Local {};

D1 *d1() { return new D1; }
D2 *d2() { return new D2; }

// CHECK-DAG: @_ZTV2D1 = {{.*}} [ptr null, ptr @_ZTI2D1, ptr @_ZN2D1D2Ev,
// CHECK-DAG: @_ZTV2D2 = {{.*}} [ptr null, ptr @_ZTI2D2, ptr @_ZN5LocalD2Ev,
// CHECK-DAG: define linkonce_odr {{.*}}void @_ZN2D1D2Ev({{.*}} !forwarder ![[F:[0-9]+]] {
// CHECK-DAG: declare {{.*}}dllimport void @_ZN8ImportedD2Ev(
// CHECK-DAG: ![[F]] = !{ptr @_ZN8ImportedD2Ev}

// ELF-NOT: define {{.*}} @_ZN2D1D2Ev(
// ELF-NOT: !forwarder
