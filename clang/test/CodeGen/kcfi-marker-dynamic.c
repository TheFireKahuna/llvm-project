// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s --check-prefix=NOMARKER --implicit-check-not=kcfi_import --implicit-check-not=kcfi.dynamic
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsanitize-kcfi-marker -emit-llvm -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=ELF --implicit-check-not=kcfi_import --implicit-check-not=kcfi.dynamic --implicit-check-not=__kcfi_inflow_ --implicit-check-not=__kcfi_param_

/// Under the KCFI marker scheme, a function declaration that is a known
/// import, dllimport or marked with an explicit default visibility that the
/// visibility mapping imports, carries !kcfi_import; one that only -fno-plt
/// imports does not. The module lists in !kcfi.dynamic the KCFI types of the
/// function pointers that may reach it from code without a prefix of ours:
/// those a conversion from another type creates, those a call to a known
/// import hands back through its return type or through a pointer parameter
/// to a non-const type, and those an exported function receives.

// NOMARKER: target triple

typedef short (*short_fn)(short);
typedef long (*long_fn)(long);
typedef float (*float_fn)(float);
typedef double (*double_fn)(double);
typedef char (*char_fn)(char);
typedef unsigned short (*ushort_fn)(unsigned short);
typedef unsigned (*uint_fn)(unsigned);
typedef long long (*llong_fn)(long long);
typedef unsigned long (*ulong_fn)(unsigned long);
typedef long double (*ldouble_fn)(long double);

/// Each function of a type the test names shares its !kcfi_type node with the
/// module's list.
// CHECK-DAG: define {{.*}} @w_short({{.*}} !kcfi_type ![[#SHORT:]]
short w_short(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_long({{.*}} !kcfi_type ![[#LONG:]]
long w_long(long x) { return x; }
// CHECK-DAG: define {{.*}} @w_float({{.*}} !kcfi_type ![[#FLOAT:]]
float w_float(float x) { return x; }
// CHECK-DAG: define {{.*}} @w_double({{.*}} !kcfi_type ![[#DOUBLE:]]
double w_double(double x) { return x; }
// CHECK-DAG: define {{.*}} @w_char({{.*}} !kcfi_type ![[#CHAR:]]
char w_char(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_uchar({{.*}} !kcfi_type ![[#UCHAR:]]
unsigned char w_uchar(unsigned char x) { return x; }
// CHECK-DAG: define {{.*}} @w_schar({{.*}} !kcfi_type ![[#SCHAR:]]
signed char w_schar(signed char x) { return x; }
// CHECK-DAG: define {{.*}} @w_ushort({{.*}} !kcfi_type ![[#USHORT:]]
unsigned short w_ushort(unsigned short x) { return x; }
// CHECK-DAG: define {{.*}} @w_uint({{.*}} !kcfi_type ![[#UINT:]]
unsigned w_uint(unsigned x) { return x; }
// CHECK-DAG: define {{.*}} @w_llong({{.*}} !kcfi_type ![[#LLONG:]]
long long w_llong(long long x) { return x; }
// CHECK-DAG: define {{.*}} @w_ulong({{.*}} !kcfi_type ![[#ULONG:]]
unsigned long w_ulong(unsigned long x) { return x; }
// CHECK-DAG: define {{.*}} @w_ldouble({{.*}} !kcfi_type ![[#LDOUBLE:]]
long double w_ldouble(long double x) { return x; }
// CHECK-DAG: define {{.*}} @w_ullong({{.*}} !kcfi_type ![[#ULLONG:]]
unsigned long long w_ullong(unsigned long long x) { return x; }
// CHECK-DAG: define {{.*}} @w_int({{.*}} !kcfi_type ![[#INT:]]
int w_int(int x) { return x; }
// CHECK-DAG: define {{.*}} @w_intv({{.*}} !kcfi_type ![[#INTV:]]
int w_intv(void) { return 0; }
// CHECK-DAG: define {{.*}} @w_bool({{.*}} !kcfi_type ![[#BOOL:]]
_Bool w_bool(_Bool x) { return x; }

struct in_holder {
  float_fn f;
};

struct ops {
  char_fn op;
};

/// A record that points to itself ends the walk.
struct node {
  struct node *next;
  struct {
    unsigned char (*f)(unsigned char);
  } inner;
  signed char (*arr[2])(signed char);
};

__attribute__((dllimport)) void *sym(const char *name);
/// Return type.
__attribute__((dllimport)) short_fn get_cb(void);
/// A pointer parameter to a non-const type.
__attribute__((dllimport)) void get_out(long_fn *out);
/// A pointer parameter to a const type is an input.
__attribute__((dllimport)) void read_in(const struct in_holder *in);
/// A function pointer passed by value flows to the callee.
__attribute__((dllimport)) void reg(double_fn cb);
/// Fields of records reached through pointers.
__attribute__((dllimport)) struct ops *get_ops(void);
__attribute__((dllimport)) struct node *head(void);
/// A declaration that only -fno-plt imports is not a known import.
ushort_fn plain_get(void);
/// A declaration with an explicit default visibility is a known import.
__attribute__((visibility("default"))) uint_fn vis_get(void);

/// Constant conversions from an integer and from the address of an object.
ulong_fn g = (ulong_fn)0x2000;
int obj;
struct ldtab {
  ldouble_fn f;
} ldt = {(ldouble_fn)&obj};

/// An exported function receives its parameters from another image.
// CHECK-DAG: define {{.*}}dllexport void @exp_def(
__attribute__((dllexport)) void exp_def(llong_fn p) {}

void use(void) {
  /// Conversions from an object pointer, an integer and a function of
  /// another type; not from a function of the same type.
  ((int (*)(void))sym("a"))();
  ((_Bool (*)(_Bool))0x1000)(1);
  ((int (*)(int))w_int)(1);
  ((unsigned long long (*)(unsigned long long))w_llong)(1);

  long_fn l;
  struct in_holder h = {w_float};
  get_cb()(1);
  get_out(&l);
  read_in(&h);
  reg(w_double);
  get_ops()->op(1);
  head()->inner.f(1);
  plain_get()(1);
  vis_get()(1);
}

// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY:]] dllimport ptr @sym(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY]] dllimport ptr @get_cb(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY]] dllimport void @get_out(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY]] dllimport void @read_in(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY]] dllimport void @reg(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY]] dllimport ptr @get_ops(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY]] dllimport ptr @head(
// CHECK-DAG: declare !kcfi_type ![[#]] dllimport ptr @plain_get(
// CHECK-DAG: declare !kcfi_type ![[#]] !kcfi_import ![[#EMPTY]] dllimport ptr @vis_get(

/// The conversions in the order the module emits them, then the parameters of
/// the exported function and the inflow of the imports in the order the
/// module declares them. FLOAT, DOUBLE, USHORT and INT are not listed.
// CHECK: !kcfi.dynamic = !{![[#ULONG]], ![[#LDOUBLE]], ![[#INTV]], ![[#BOOL]], ![[#ULLONG]], ![[#LLONG]], ![[#SHORT]], ![[#LONG]], ![[#CHAR]], ![[#UCHAR]], ![[#SCHAR]], ![[#UINT]]}
// CHECK: ![[#EMPTY]] = !{}

/// ELF targets record none of these facts.
// ELF: target triple
