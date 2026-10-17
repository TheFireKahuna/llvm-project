// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++20 -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -ffunction-type-prefix -emit-llvm -o - %s | FileCheck %s --check-prefix=ELF --implicit-check-not=kcfi_import --implicit-check-not=kcfi.dynamic --implicit-check-not=__kcfi_inflow_ --implicit-check-not=__kcfi_param_

/// Under the KCFI marker scheme, a function pointer that code can read or
/// write untyped opens its type to functions without a prefix of ours: when a
/// pointer to it is converted to another pointer type, as in
/// *(void **)&fp = dlsym(...) or memcpy(&fp, ...), when it is reinterpreted
/// as an object of another type, or an object of another type as it, and when
/// another value is bit-cast to it. A function pointer converted to an object
/// pointer flows out and opens nothing.

short w_short(short x) { return x; }
// CHECK-DAG: define {{.*}} @_Z7w_shorts({{.*}} !kcfi_type ![[#SHORT:]]
long w_long(long x) { return x; }
// CHECK-DAG: define {{.*}} @_Z6w_longl({{.*}} !kcfi_type ![[#LONG:]]
char w_char(char x) { return x; }
// CHECK-DAG: define {{.*}} @_Z6w_charc({{.*}} !kcfi_type ![[#CHAR:]]
double w_double(double x) { return x; }
// CHECK-DAG: define {{.*}} @_Z8w_doubled({{.*}} !kcfi_type ![[#DOUBLE:]]
float w_float(float x) { return x; }
// CHECK-DAG: define {{.*}} @_Z7w_floatf({{.*}} !kcfi_type ![[#FLOAT:]]
unsigned w_uint(unsigned x) { return x; }
// CHECK-DAG: define {{.*}} @_Z6w_uintj({{.*}} !kcfi_type ![[#UINT:]]

extern "C" void *memcpy(void *, const void *, decltype(sizeof 0));
void *sym();

void *use(const void *src, unsigned long long n, void **vpp) {
  short (*f1)(short);
  *(void **)&f1 = sym();
  f1(1);

  long (*f2)(long);
  memcpy(&f2, src, sizeof f2);
  f2(1);

  char (*f3)(char) = __builtin_bit_cast(char (*)(char), n);
  f3(1);

  double (*f4)(double);
  reinterpret_cast<void *&>(f4) = sym();
  f4(1);

  (*(float (**)(float))vpp)(1);

  unsigned (*f6)(unsigned) = w_uint;
  return (void *)f6;
}

/// UINT is not listed.
// CHECK: !kcfi.dynamic = !{![[#SHORT]], ![[#LONG]], ![[#CHAR]], ![[#DOUBLE]], ![[#FLOAT]]}

/// ELF targets record none of these facts.
// ELF: target triple
