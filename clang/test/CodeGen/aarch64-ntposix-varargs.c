// REQUIRES: aarch64-registered-target
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -triple aarch64-pc-windows-msvc -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -O1 -S -o - %s \
// RUN:   | FileCheck %s --check-prefix=ASM

// NT-POSIX on AArch64 uses the Windows ARM64 variadic convention, as the
// backend does: va_list is a plain pointer, and variadic arguments, floating
// point included, travel in the general-purpose registers.
// __builtin_ms_va_start is valid in any function.

double f(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  double d = __builtin_va_arg(ap, double);
  __builtin_va_end(ap);
  return d;
}
// IR-LABEL: define dso_local double @f(i32 noundef %n, ...)
// IR:         %ap = alloca ptr, align 8
// IR:         call void @llvm.va_start.p0(ptr %ap)
// IR-NEXT:    %argp.cur = load ptr, ptr %ap, align 8
// IR-NEXT:    %argp.next = getelementptr inbounds i8, ptr %argp.cur, i64 8
// IR-NEXT:    store ptr %argp.next, ptr %ap, align 8
// IR-NEXT:    load double, ptr %argp.cur, align 8

// ASM-LABEL: f:
// ASM-NOT:     q0
// ASM:         fmov d0, x1
// ASM:         ret

double g(void) { return f(1, 2.0); }
// IR-LABEL: define dso_local double @g()
// IR:         call double (i32, ...) @f(i32 noundef 1, double noundef 2.000000e+00)

// ASM-LABEL: g:
// ASM:         mov x1, #4611686018427387904
// ASM-NEXT:    b f

double h(int n, ...) {
  __builtin_ms_va_list ap;
  __builtin_ms_va_start(ap, n);
  double d = __builtin_va_arg(ap, double);
  __builtin_ms_va_end(ap);
  return d;
}
// IR-LABEL: define dso_local double @h(i32 noundef %n, ...)
// IR:         %ap = alloca ptr, align 8
// IR:         call void @llvm.va_start.p0(ptr %ap)
