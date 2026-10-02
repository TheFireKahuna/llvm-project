// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fms-extensions -fsanitize-kcfi-marker -emit-llvm -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=ELF --implicit-check-not=kcfi_import --implicit-check-not=kcfi.dynamic --implicit-check-not=__kcfi_inflow_ --implicit-check-not=__kcfi_param_

/// Under the KCFI marker scheme, a virtual call on a class that another image
/// may create objects of, a class without hidden visibility or one that
/// derives from a class with a uuid, opens the type of its slot to functions
/// without a prefix of ours, destructor slots included. A call through a
/// member function pointer to such a class opens the "__vfn" type and the
/// ordinary type. A polymorphic class that a known import hands back opens
/// the types of all its slots. A hidden class opens nothing.

short w_short(short x) { return x; }
// CHECK-DAG: define {{.*}} @_Z7w_shorts({{.*}} !kcfi_type ![[#SHORT:]]
int w_int(int x) { return x; }
// CHECK-DAG: define {{.*}} @_Z5w_inti({{.*}} !kcfi_type ![[#INT:]]
double w_double(double x) { return x; }
// CHECK-DAG: define {{.*}} @_Z8w_doubled({{.*}} !kcfi_type ![[#DOUBLE:]]

struct Pub {
  virtual short f(short);
  virtual ~Pub();
};
short Pub::f(short x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN3Pub1fEs({{.*}} !kcfi_type ![[#PUBF:]] !kcfi_vfn_type ![[#VFN:]]
Pub::~Pub() {}
// CHECK-DAG: define {{.*}} @_ZN3PubD2Ev({{.*}} !kcfi_type ![[#DTOR:]]
// CHECK-DAG: define {{.*}} @_ZN3PubD0Ev({{.*}} !kcfi_type ![[#DELPUB:]]

struct __attribute__((visibility("hidden"))) Hid {
  virtual int g(int);
};
int Hid::g(int x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN3Hid1gEi({{.*}} !kcfi_type ![[#HIDG:]]

struct __declspec(uuid("00000000-0000-0000-c000-000000000046")) IUnk {
  virtual long q(long) = 0;
};
struct __attribute__((visibility("hidden"))) IFace : IUnk {
  virtual char m(char) = 0;
};
struct __attribute__((visibility("hidden"))) Impl : IFace {
  long q(long) override;
  char m(char) override;
};
long Impl::q(long x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN4Impl1qEl({{.*}} !kcfi_type ![[#QSLOT:]]
char Impl::m(char c) { return c; }
// CHECK-DAG: define {{.*}} @_ZN4Impl1mEc({{.*}} !kcfi_type ![[#MSLOT:]]

struct Fac {
  virtual unsigned u(unsigned);
  virtual ~Fac();
};
unsigned Fac::u(unsigned x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN3Fac1uEj({{.*}} !kcfi_type ![[#FACU:]]
Fac::~Fac() {}
// CHECK-DAG: define {{.*}} @_ZN3FacD0Ev({{.*}} !kcfi_type ![[#DELFAC:]]

__attribute__((dllimport)) Fac *make_fac();
__attribute__((dllimport)) IFace *create();

struct Arg {
  virtual float a(float);
};

/// An exported member function receives its parameters from another image;
/// a polymorphic class that a parameter points to adds none of its slots.
struct __attribute__((dllexport)) Exp {
  void set(double (*cb)(double));
  void take(Arg *a);
};
void Exp::set(double (*cb)(double)) {}
void Exp::take(Arg *a) {}

void calls(Pub *p, Hid *h, IFace *i, short (Pub::*pm)(short),
           int (Hid::*hm)(int), void *vp) {
  p->f(1);
  delete p;
  h->g(1);
  i->m('a');
  (p->*pm)(1);
  (h->*hm)(1);
  /// A conversion that reinterprets an object as a function pointer.
  reinterpret_cast<int (*&)(int)>(vp)(1);
  make_fac();
  create();
}

// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#]] dllimport {{.*}}@_Z8make_facv(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#]] dllimport {{.*}}@_Z6createv(

/// The calls in the order the module emits them, then the parameters of the
/// exported function and the slots of the classes the imports hand back.
/// HIDG is not listed.
// CHECK: !kcfi.dynamic = !{![[#PUBF]], ![[#DELPUB]], ![[#MSLOT]], ![[#VFN]], ![[#SHORT]], ![[#INT]], ![[#DOUBLE]], ![[#FACU]], ![[#DTOR]], ![[#DELFAC]], ![[#QSLOT]]}

/// ELF targets record none of these facts.
// ELF: target triple
