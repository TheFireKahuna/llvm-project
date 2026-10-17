// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefix=USER %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefix=USER %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -DOWNER -o - \
// RUN:   %s | FileCheck --check-prefix=OWNER %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -o - %s | FileCheck --check-prefix=ELF %s

/// An image that constructs a class whose vtable another image owns, and that
/// a derived class may extend, emits a discardable copy of the vtable, which
/// it reaches through the vtable's import pointer and does not export; the
/// linker binds the pointer to the copy. The owner keeps its vtable's external
/// linkage in the copies' COMDAT, as it keeps its VTT and construction vtables.
/// A final class's vtable is imported, and a
/// class the mapping does not export, or marked dllimport, has no copy.

#define API __attribute__((visibility("default")))

struct API B {
  virtual ~B();
  virtual int f() { return 1; }
};
struct API F final : B {
  ~F() override;
};
struct Hidden {
  virtual ~Hidden();
};
#ifdef _WIN32
#define IMPORT __attribute__((dllimport))
#else
#define IMPORT
#endif
struct API IMPORT Imported {
  virtual ~Imported();
};

struct API V : virtual B {
  V() {}
  virtual void g();
};
struct API W : V {
  W() {}
  virtual void h();
};

#ifdef OWNER
B::~B() {}
void W::h() {}
#endif
W *w() { return new W; }
B *b() { return new B; }
F *f() { return new F; }
Hidden *h() { return new Hidden; }
Imported *i() { return new Imported; }

// USER-DAG: $_ZTV1B = comdat any
// USER-DAG: @_ZTV1B = linkonce_odr unnamed_addr constant {{.*}}, comdat, align 64
// USER-DAG: @_ZTV1F = external constant
// USER-DAG: @_ZTV6Hidden = external dso_local constant
// USER-DAG: @"_ZTV1B$ap16" = unnamed_addr alias
// USER-DAG: @_ZTT1W = linkonce_odr dso_local unnamed_addr constant {{.*}}, comdat
// USER-NOT: @_ZTV1B = {{.*}}dllexport
// USER-NOT: @_ZTV8Imported = linkonce_odr

// OWNER-DAG: @_ZTV1B = dso_local dllexport constant {{.*}}, comdat, align 64
// OWNER-DAG: @_ZTV1W = dso_local dllexport constant {{.*}}, comdat
// OWNER-DAG: @_ZTT1W = dso_local dllexport unnamed_addr constant {{.*}}, comdat
// OWNER-DAG: @_ZTC1W0_1V = dso_local dllexport constant {{.*}}, comdat

// ELF: @_ZTV1B = external constant
