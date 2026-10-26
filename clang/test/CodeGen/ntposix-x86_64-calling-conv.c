// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -mstack-probe-size=8192 \
// RUN:   -emit-llvm -o - %s | FileCheck %s

// x86-64 NT-POSIX's default convention is System V; ms_abi names Microsoft's.
// Stack probe options are those of Windows.

struct S {
  long long a, b;
};

// CHECK-LABEL: define dso_local i64 @by_value(i64 %s.coerce0, i64 %s.coerce1)
// CHECK-SAME:    [[ATTRS:#[0-9]+]]
long long by_value(struct S s) { return s.a; }

// CHECK-LABEL: define dso_local win64cc i64 @ms_by_value(ptr
long long __attribute__((ms_abi)) ms_by_value(struct S s) { return s.a; }

// CHECK-LABEL: define dso_local i64 @sysv_by_value(i64 %s.coerce0, i64 %s.coerce1)
long long __attribute__((sysv_abi)) sysv_by_value(struct S s) { return s.a; }

// CHECK-LABEL: define dso_local i32 @variadic(
// CHECK:         %ap = alloca [1 x %struct.__va_list_tag], align 16
int variadic(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  int i = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return i;
}

// A call without a prototype is variadic, so that it sets %al.
// CHECK-LABEL: define dso_local i32 @call_no_proto()
// CHECK:         call i32 (double, ...) @no_proto(double noundef 1.000000e+00)
int no_proto();
int call_no_proto(void) { return no_proto(1.0); }

// CHECK: attributes [[ATTRS]] = { {{.*}}"stack-probe-size"="8192"
