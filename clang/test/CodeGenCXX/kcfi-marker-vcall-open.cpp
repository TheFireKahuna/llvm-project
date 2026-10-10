// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -flto -fwhole-program-vtables -emit-llvm -o - %s | FileCheck %s --check-prefix=LTO

/// A virtual call opens the type of its slot only when the class it is made
/// through, or one of its bases, says that another image or foreign code may
/// create its objects: an explicit default visibility that the visibility
/// mapping exports, dllimport, dllexport or a uuid. An implicit visibility
/// says nothing, and a hidden class opens nothing either. LTO visibility does
/// not follow: a class with an implicit visibility keeps public LTO
/// visibility, as on ELF.

struct Impl {
  virtual char a(char);
};
struct __attribute__((visibility("default"))) Vis {
  virtual short b(short);
};
struct __declspec(dllimport) Imp {
  virtual int c(int);
};
struct __declspec(dllexport) Exp {
  virtual long d(long);
};
struct __declspec(uuid("00000000-0000-0000-c000-000000000046")) IUnk {
  virtual float e(float) = 0;
};
/// A class with an implicit visibility that derives from one that says so.
struct FromUnk : IUnk {
  virtual double f(double);
};
struct FromVis : Vis {
  virtual unsigned g(unsigned);
};
struct __attribute__((visibility("hidden"))) Hid {
  virtual long long h(long long);
};

char Impl::a(char x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN4Impl1aEc({{.*}} !kcfi_type ![[#A:]]
short Vis::b(short x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN3Vis1bEs({{.*}} !kcfi_type ![[#B:]] !kcfi_vfn_type ![[#VFN:]]
long Exp::d(long x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN3Exp1dEl({{.*}} !kcfi_type ![[#D:]]
double FromUnk::f(double x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN7FromUnk1fEd({{.*}} !kcfi_type ![[#F:]]
unsigned FromVis::g(unsigned x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN7FromVis1gEj({{.*}} !kcfi_type ![[#G:]]
long long Hid::h(long long x) { return x; }
// CHECK-DAG: define {{.*}} @_ZN3Hid1hEx({{.*}} !kcfi_type ![[#H:]]

void calls(Impl *a, Vis *b, Imp *c, Exp *d, FromUnk *f, FromVis *g, Hid *h,
           char (Impl::*am)(char), short (Vis::*bm)(short)) {
  a->a('a');
  b->b(1);
  c->c(1);
  d->d(1);
  f->f(1);
  g->g(1);
  h->h(1);
  (a->*am)('a');
  (b->*bm)(1);
}

// LTO: call i1 @llvm.public.type.test(ptr %{{.*}}, metadata !"_ZTS4Impl")
// LTO: call i1 @llvm.type.test(ptr %{{.*}}, metadata !"_ZTS3Hid")

/// Imp::c is only declared, so its slot type C is matched by position. The
/// member pointer call through Vis opens the "__vfn" type and the ordinary
/// type of short(short). A and H are not listed, nor is the "__vfn" type of
/// Impl::a.
// CHECK: !kcfi.dynamic = !{![[#B]], ![[#C:]], ![[#D]], ![[#F]], ![[#G]], ![[#VFN]], ![[#SHORT:]]}
