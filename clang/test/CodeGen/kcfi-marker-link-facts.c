// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -x c++ -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s --check-prefix=NOMARKER --implicit-check-not=__kcfi_inflow_ --implicit-check-not=__kcfi_param_
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ffunction-type-prefix -fsanitize=kcfi -emit-llvm -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=ELF --implicit-check-not=kcfi_import --implicit-check-not=kcfi.dynamic --implicit-check-not=__kcfi_inflow_ --implicit-check-not=__kcfi_param_

/// Under the KCFI marker scheme, a C function that may be foreign code the
/// linker brings into the image gives the linker the KCFI types it would open,
/// as weak constants named after the type and the function, in the form of the
/// __kcfi_typeid_ constants. A referenced declaration that is not a known
/// import gives the types of the function pointers a call to it hands back,
/// as __kcfi_inflow_<type>_<function>; a definition that is not exported gives
/// the types of the function pointers its parameters can hold, as
/// __kcfi_param_<type>_<function>. Known imports and exported definitions
/// open their types dynamically instead, and functions with C++ linkage give
/// nothing.

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*int_fn)(int);
typedef long (*long_fn)(long);
typedef short (*short_fn)(short);

/// Address-taken declarations give each type's value as __kcfi_typeid_.
int t_int(int);
long t_long(long);
short t_short(short);
__attribute__((used)) static int_fn take_int = t_int;
__attribute__((used)) static long_fn take_long = t_long;
__attribute__((used)) static short_fn take_short = t_short;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_int"
// CHECK-NEXT: ".set __kcfi_typeid_t_int, [[#%u,INT:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_long"
// CHECK-NEXT: ".set __kcfi_typeid_t_long, [[#%u,LONG:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_short"
// CHECK-NEXT: ".set __kcfi_typeid_t_short, [[#%u,SHORT:]] /* {{.*}} */"

/// A definition that is not exported gives the types its parameters can hold,
/// a function pointer passed by value among them.
// CHECK-NEXT: ".weak __kcfi_param_00000000[[#%.8x,SHORT]]_def_cb"
// CHECK-NEXT: ".set __kcfi_param_00000000[[#%.8x,SHORT]]_def_cb, {{[0-9]+}}"
void def_cb(short_fn cb) {}

/// An exported definition opens its types dynamically.
__attribute__((dllexport)) void exp_cb(long_fn cb) {}

/// A function with internal linkage cannot be called by foreign code.
static void local_def(int_fn cb) {}

/// A declaration that only -fno-plt imports is not a known import: it gives
/// the types in its return type and behind its pointer parameters to non-const
/// types.
int_fn foreign_get(void);
void foreign_out(long_fn *out);
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,INT]]_foreign_get"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,INT]]_foreign_get, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,LONG]]_foreign_out"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,LONG]]_foreign_out, {{[0-9]+}}"

/// A function pointer passed by value flows to the callee.
void foreign_reg(short_fn cb);
/// A known import opens its types dynamically.
__attribute__((dllimport)) short_fn imp_get(void);
/// A declaration that is not referenced gives nothing.
long_fn unused_get(void);

void use(void) {
  long_fn l;
  foreign_get();
  foreign_out(&l);
  foreign_reg(t_short);
  imp_get();
  local_def(t_int);
}

#ifdef __cplusplus
}

int_fn cxx_get();
void cxx_def(int_fn cb) {}
void cxx_use() { cxx_get(); }
#endif

// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

// NOMARKER: ".weak __kcfi_typeid_t_int"

/// ELF targets record none of these facts.
// ELF: target triple
