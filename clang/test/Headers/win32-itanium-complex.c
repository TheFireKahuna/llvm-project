// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=X64
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -D_DLL -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=A64

// In C, complex.h declares the UCRT's complex functions over the C99 _Complex
// types, passed as the UCRT's structures of the same element type are.

#include <complex.h>

float complex f(float complex z) { return cexpf(z) * I; }
double d(double complex z) { return cabs(z) + creal(CMPLX(1.0, 2.0)); }
long double complex l(long double complex z) { return csqrtl(z); }

// X64: declare dllimport i64 @cexpf(i64 noundef)
// X64: declare dllimport double @cabs(ptr {{.*}}dereferenceable(16))
// X64: declare dllimport void @csqrtl(ptr {{.*}}sret({ double, double }) align 8, ptr {{.*}}dereferenceable(16))

// A64: declare dllimport { float, float } @cexpf([2 x float] noundef)
// A64: declare dllimport double @cabs([2 x double] noundef)
// A64: declare dllimport { double, double } @csqrtl([2 x double] noundef)
