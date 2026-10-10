// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s

/// A virtual or member function pointer call whose type opens because
/// another image may create objects of its class hands back the function
/// pointers held in what it returns and in what it stores through its
/// out-parameters, and the callee may call the functions of ours that the
/// call hands it with pointers to its own functions, so those types open as
/// they do for a call through a function pointer that opens. A call through
/// a class that opens nothing gives nothing.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);

struct a_ops {
  a_fn f;
};
struct d_ops {
  d_fn f;
};
typedef void (*cb_c)(c_fn);
typedef void (*cb_f)(f_fn);

struct __declspec(dllimport) Imp {
  virtual a_ops *get();
  virtual void out(b_fn *p);
  virtual void reg(cb_c cb);
};
struct __declspec(dllimport) ImpM {
  void take(e_fn *p);
};
struct Mine {
  virtual d_ops *get();
  virtual void reg(cb_f cb);
};

static void my_c(c_fn) {}
static void my_f(f_fn) {}

void calls(Imp *i, ImpM *im, void (ImpM::*mp)(e_fn *), Mine *m) {
  b_fn b;
  e_fn e;
  i->get()->f('a');
  i->out(&b);
  i->reg(my_c);
  (im->*mp)(&e);
  m->get()->f(1);
  m->reg(my_f);
}

// CHECK-DAG: define {{.*}} @_Z3w_ac({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @_Z3w_bs({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @_Z3w_ci({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }
// CHECK-DAG: define {{.*}} @_Z3w_ef({{.*}} !kcfi_type ![[#E:]]
float w_e(float x) { return x; }

/// The slot types of Imp's three calls and the two types of the member
/// function pointer call, then what those calls hand back. D and F are not
/// listed.
// CHECK: !kcfi.dynamic = !{![[#]], ![[#]], ![[#]], ![[#]], ![[#]], ![[#A]], ![[#B]], ![[#C]], ![[#E]]}
