// DEFINE: %{cc} = %clang_cc1 -fdeclspec -std=c++17 -O1 -mconstructor-aliases -disable-llvm-passes -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck %s
// RUN: %{cc} -triple x86_64-w64-mingw32 | FileCheck --check-prefix=MINGW %s

/// A base destructor that does nothing beyond that of a base class another
/// image may define is not given a body that calls it: it is a weak external
/// whose alternate is the base class's, and the complete destructor with it,
/// so that the linker binds every reference, the vtable's among them, to the
/// base class's destructor. Other units name both, so both are kept. A
/// discardable destructor is still replaced by the base class's within the
/// unit, and one whose base class's is defined here is still an alias.

struct Imported {
  __declspec(dllimport) virtual ~Imported();
};
struct Strong : Imported {
  ~Strong() override;
  virtual void key();
};
Strong::~Strong() {}
void Strong::key() {}

template <typename T> struct Tmpl : Imported {
  ~Tmpl() override {}
};
template struct Tmpl<int>;

struct Inline : Imported {};
Imported *i() { return new Inline; }

struct Local {
  virtual ~Local();
};
Local::~Local() {}
struct OfLocal : Local {
  ~OfLocal() override;
};
OfLocal::~OfLocal() {}

// CHECK-DAG: @_ZTV6Strong = {{.*}} ptr @_ZN6StrongD1Ev, ptr @_ZN6StrongD0Ev,
// CHECK-DAG: @_ZTV6Inline = {{.*}} ptr @_ZN8ImportedD2Ev, ptr @_ZN6InlineD0Ev]
// CHECK-DAG: @llvm.compiler.used = appending global [4 x ptr] [ptr @_ZN6StrongD2Ev, ptr @_ZN6StrongD1Ev, ptr @_ZN4TmplIiED2Ev, ptr @_ZN4TmplIiED1Ev]
// CHECK-DAG: @_ZN7OfLocalD2Ev = dso_local unnamed_addr alias void (ptr), ptr @_ZN5LocalD2Ev
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN6StrongD2Ev({{.*}}) unnamed_addr #[[FWD:[0-9]+]]
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN6StrongD1Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN4TmplIiED2Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN4TmplIiED1Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: attributes #[[FWD]] = { {{.*}}"weak-alternate"="_ZN8ImportedD2Ev" }

// MINGW: define dso_local void @_ZN6StrongD2Ev(
// MINGW-NOT: "weak-alternate"
