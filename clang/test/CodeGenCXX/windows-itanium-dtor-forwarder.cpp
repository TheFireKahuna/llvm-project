// DEFINE: %{cc} = %clang_cc1 -fdeclspec -std=c++17 -O1 -mconstructor-aliases -disable-llvm-passes -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit | FileCheck --check-prefix=MAP %s
// RUN: %{cc} -triple x86_64-w64-mingw32 | FileCheck --check-prefix=MINGW %s

/// A base destructor that does nothing beyond that of a base class another
/// image may define is not given a body that calls it: it is a weak external
/// whose alternate is the base class's, and the complete destructor with it, so
/// that the linker binds every reference, the vtable's among them, to the base
/// class's destructor. Other units name both, so both are kept. A discardable
/// destructor is still replaced by the base class's within the unit, and one
/// whose base class's is defined here is still an alias. A base class's
/// destructor that the unit only declares is treated as one that another image
/// defines, whether or not it is imported: without the mapping, VisBase's is a
/// plain declaration. The weak external is the image's definition of the
/// destructor, so it is exported where its class is, and is never an import,
/// though the export mapping imports the declaration of a class with explicit
/// visibility. Its alternate is the first base destructor that does more than
/// call its base's as far as the unit has defined the ones between, and
/// otherwise one of those, whose own weak external the linker follows. A
/// weak_odr destructor whose base class's is defined here is an alias of it,
/// which COFF lowers to the same weak external, whatever that one's linkage; a
/// strong one whose base class's is weak keeps its body.

struct Imported {
  __declspec(dllimport) virtual ~Imported();
};
struct Strong : Imported {
  ~Strong() override;
  virtual void key();
};
Strong::~Strong() {}
void Strong::key() {}

struct __declspec(dllexport) Exported : Imported {
  ~Exported() override;
};
Exported::~Exported() {}

struct __attribute__((visibility("default"))) VisBase {
  virtual ~VisBase();
};
struct __attribute__((visibility("default"))) Vis : VisBase {
  ~Vis() override;
};
Vis::~Vis() {}
struct __attribute__((visibility("default"))) Chain : Vis {
  ~Chain() override;
};
Chain::~Chain() {}

struct __attribute__((visibility("default"))) Late : VisBase {
  ~Late() override;
};
struct __attribute__((visibility("default"))) Early : Late {
  ~Early() override;
};
Early::~Early() {}
Late::~Late() {}

template <typename T> struct Tmpl : Imported {
  ~Tmpl() override {}
};
template struct Tmpl<int>;

struct Inline : Imported {};
Imported *i() { return new Inline; }

struct OverInline : Inline {
  ~OverInline() override;
};
OverInline::~OverInline() {}

struct Local {
  virtual ~Local();
};
Local::~Local() {}
struct OfLocal : Local {
  ~OfLocal() override;
};
OfLocal::~OfLocal() {}

template <typename T> struct WeakBase {
  virtual ~WeakBase() {}
};
template <typename T> struct OfWeak : WeakBase<T> {
  ~OfWeak() override {}
};
template struct WeakBase<int>;
template struct OfWeak<int>;

template <typename T> struct WeakOfLocal : Local {
  ~WeakOfLocal() override {}
};
template struct WeakOfLocal<int>;

struct StrongOfWeak : WeakBase<long> {
  ~StrongOfWeak() override;
};
template struct WeakBase<long>;
StrongOfWeak::~StrongOfWeak() {}

// CHECK-DAG: @_ZTV6Strong = {{.*}} ptr @_ZN6StrongD1Ev, ptr @_ZN6StrongD0Ev,
// CHECK-DAG: @_ZTV6Inline = {{.*}} ptr @_ZN8ImportedD2Ev, ptr @_ZN6InlineD0Ev]
// CHECK-DAG: @llvm.compiler.used = appending global [16 x ptr] [ptr @_ZN6StrongD2Ev, ptr @_ZN6StrongD1Ev, ptr @_ZN8ExportedD2Ev, ptr @_ZN8ExportedD1Ev, ptr @_ZN3VisD2Ev, ptr @_ZN3VisD1Ev, ptr @_ZN5ChainD2Ev, ptr @_ZN5ChainD1Ev, ptr @_ZN5EarlyD2Ev, ptr @_ZN5EarlyD1Ev, ptr @_ZN4LateD2Ev, ptr @_ZN4LateD1Ev, ptr @_ZN4TmplIiED2Ev, ptr @_ZN4TmplIiED1Ev, ptr @_ZN10OverInlineD2Ev, ptr @_ZN10OverInlineD1Ev]
// CHECK-DAG: @_ZN7OfLocalD2Ev = dso_local unnamed_addr alias void (ptr), ptr @_ZN5LocalD2Ev
// CHECK-DAG: @_ZN6OfWeakIiED2Ev = weak_odr dso_local unnamed_addr alias void (ptr), ptr @_ZN8WeakBaseIiED2Ev
// CHECK-DAG: @_ZN6OfWeakIiED1Ev = weak_odr dso_local unnamed_addr alias void (ptr), ptr @_ZN6OfWeakIiED2Ev
// CHECK-DAG: @_ZN11WeakOfLocalIiED2Ev = weak_odr dso_local unnamed_addr alias void (ptr), ptr @_ZN5LocalD2Ev
// CHECK-DAG: define dso_local void @_ZN12StrongOfWeakD2Ev(
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN6StrongD2Ev({{.*}}) unnamed_addr #[[FWD:[0-9]+]]
// CHECK-DAG: declare {{.*}}extern_weak dllexport void @_ZN8ExportedD2Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: declare {{.*}}extern_weak dllexport void @_ZN8ExportedD1Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN6StrongD1Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN4TmplIiED2Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN4TmplIiED1Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN10OverInlineD2Ev({{.*}}) unnamed_addr #[[FWD]]
// CHECK-DAG: attributes #[[FWD]] = { {{.*}}"weak-alternate"="_ZN8ImportedD2Ev" }
// CHECK-DAG: declare {{.*}}extern_weak void @_ZN5ChainD2Ev({{.*}}) unnamed_addr #[[PLAIN:[0-9]+]]
// CHECK-DAG: attributes #[[PLAIN]] = { {{.*}}"weak-alternate"="_ZN7VisBaseD2Ev" }

// MAP-DAG: declare {{.*}}extern_weak void @_ZN6StrongD2Ev(
// MAP-DAG: declare {{.*}}extern_weak dllexport void @_ZN3VisD2Ev({{.*}}) unnamed_addr #[[VIS:[0-9]+]]
// MAP-DAG: declare {{.*}}extern_weak dllexport void @_ZN3VisD1Ev({{.*}}) unnamed_addr #[[VIS]]
// MAP-DAG: declare {{.*}}extern_weak dllexport void @_ZN5ChainD2Ev({{.*}}) unnamed_addr #[[VIS]]
// MAP-DAG: declare {{.*}}extern_weak dllexport void @_ZN5EarlyD2Ev({{.*}}) unnamed_addr #[[LATE:[0-9]+]]
// MAP-DAG: declare {{.*}}extern_weak dllexport void @_ZN4LateD2Ev({{.*}}) unnamed_addr #[[VIS]]
// MAP-DAG: define {{.*}}dllexport void @_ZN3VisD0Ev(
// MAP-DAG: call void @_ZN3VisD1Ev(
// MAP-DAG: attributes #[[VIS]] = { {{.*}}"weak-alternate"="_ZN7VisBaseD2Ev" }
// MAP-DAG: attributes #[[LATE]] = { {{.*}}"weak-alternate"="_ZN4LateD2Ev" }

// MINGW: define dso_local void @_ZN6StrongD2Ev(
// MINGW-NOT: "weak-alternate"
// MINGW: define weak_odr dso_local void @_ZN6OfWeakIiED2Ev(
// MINGW-NOT: "weak-alternate"
