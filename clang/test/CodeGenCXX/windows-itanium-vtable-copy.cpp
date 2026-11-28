// DEFINE: %{cc} = %clang_cc1 -std=c++17 -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-pc-windows-ntposix | FileCheck --check-prefix=USER %s
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck --check-prefix=USER %s
// RUN: %{cc} -triple x86_64-pc-windows-ntposix -pic-level 2 | FileCheck --check-prefix=USER %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -DOWNER | FileCheck --check-prefix=OWNER %s
// RUN: %{cc} -triple aarch64-unknown-windows-itanium -DOWNER | FileCheck --check-prefix=OWNER %s
// RUN: %{cc} -triple x86_64-w64-mingw32 | FileCheck --check-prefix=MINGW %s
// RUN: %{cc} -triple x86_64-pc-windows-ntposix -mdefault-visibility-export-mapping=all | FileCheck --check-prefix=ALL %s

/// A unit that constructs a class whose vtable another unit owns and exports,
/// and that a derived class may extend, emits a discardable copy of the
/// vtable, with its VTT and construction vtables, and reaches the copy as it
/// would reach the owner's vtable, so it is not dso_local. The copy names the
/// class's inline and implicit virtual members, which the unit defines. The
/// owner keeps external linkage in the copies' COMDATs, also where its own
/// unit would make no copy, since another unit's options and marks decide
/// that. A final class, one the mapping does not export and one marked
/// dllimport have no copy, and the vtable and VTT of one marked dllimport are
/// available_externally, in no COMDAT. The vtable of a final class that the
/// mapping exports is not dso_local whatever the access to external data is,
/// and that of a class it does not export is.

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
struct API __attribute__((dllimport)) Imported {
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
struct API Implicit : B {
  virtual void key();
};
template <typename T> struct API Tmpl : B {
  virtual void t();
};
extern template struct Tmpl<int>;
template <typename T> struct API ImportedTmpl : virtual B {
  ImportedTmpl() {}
};
template struct __attribute__((dllimport)) ImportedTmpl<int>;

#ifdef OWNER
B::~B() {}
void W::h() {}
F::~F() {}
Hidden::~Hidden() {}
#endif
B *b() { return new B; }
F *f() { return new F; }
Hidden *h() { return new Hidden; }
Imported *i() { return new Imported; }
W *w() { return new W; }
Implicit *m() { return new Implicit; }
Tmpl<int> *t() { return new Tmpl<int>; }

// USER-DAG: @_ZTV1B = linkonce_odr {{(dllexport )?}}unnamed_addr constant {{.*}} ptr @_ZTI1B, {{.*}}, comdat,
// USER-DAG: @_ZTI1B = external {{.*}}constant ptr
// USER-DAG: @_ZTV1F = external constant
// USER-DAG: @_ZTV6Hidden = external dso_local constant
// USER-DAG: declare {{.*}}dllimport void @_ZN8ImportedC1Ev(
// USER-DAG: @_ZTV1W = linkonce_odr {{(dllexport )?}}unnamed_addr constant
// USER-DAG: @_ZTT1W = linkonce_odr dso_local {{.*}}constant {{.*}}, comdat,
// USER-DAG: @_ZTC1W0_1V = linkonce_odr dso_local {{.*}}constant {{.*}}, comdat,
// USER-DAG: @_ZTV8Implicit = linkonce_odr {{.*}} ptr @_ZN8ImplicitD1Ev, ptr @_ZN8ImplicitD0Ev,
// USER-DAG: @_ZTV4TmplIiE = linkonce_odr {{.*}} ptr @_ZN4TmplIiED1Ev, ptr @_ZN4TmplIiED0Ev,
// USER-DAG: define linkonce_odr {{.*}} @_ZN1B1fEv(
// USER-DAG: define linkonce_odr {{.*}} @_ZN8ImplicitD1Ev(
// USER-DAG: define linkonce_odr {{.*}} @_ZN8ImplicitD0Ev(
// USER-DAG: define linkonce_odr {{.*}} @_ZN4TmplIiED1Ev(
// USER-DAG: define linkonce_odr {{.*}} @_ZN4TmplIiED0Ev(
// USER-DAG: @_ZTV12ImportedTmplIiE = available_externally dllimport constant {{.*}}, align 8{{$}}
// USER-DAG: @_ZTT12ImportedTmplIiE = available_externally dllimport unnamed_addr constant {{.*}}, align 8{{$}}

// OWNER-DAG: @_ZTV1B = dso_local dllexport constant {{.*}}, comdat,
// OWNER-DAG: @_ZTV1W = dso_local dllexport constant {{.*}}, comdat,
// OWNER-DAG: @_ZTT1W = dso_local dllexport unnamed_addr constant {{.*}}, comdat,
// OWNER-DAG: @_ZTC1W0_1V = dso_local {{.*}}constant {{.*}}, comdat,
// OWNER-DAG: @_ZTV1V = linkonce_odr {{.*}}constant
// OWNER-DAG: @_ZTV6Hidden = dso_local constant {{.*}}, comdat,
// OWNER-DAG: @_ZTV1F = dso_local dllexport constant {{.*}} }, align 8{{$}}

// ALL-DAG: @_ZTV6Hidden = linkonce_odr unnamed_addr constant {{.*}}, comdat,
// ALL-DAG: @_ZTV1F = external constant

// MINGW-DAG: @_ZTV1B = external {{.*}}constant
// MINGW-DAG: @_ZTV8Implicit = external {{.*}}constant
