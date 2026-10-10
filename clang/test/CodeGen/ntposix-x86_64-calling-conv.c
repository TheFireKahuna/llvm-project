// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fms-extensions \
// RUN:   -mstack-probe-size=8192 -mno-stack-arg-probe -emit-llvm -o - %s \
// RUN:   | FileCheck %s

// x86-64 NT-POSIX uses the System V convention by default, which the backend
// lowers the C calling convention to, so no function carries x86_64_sysvcc.
// ms_abi selects the Microsoft x64 convention. Linker directives and stack
// probe options are those of Windows.

#pragma comment(lib, "ntdll")
#pragma detect_mismatch("key", "value")

struct S {
  long long a, b;
};

long long by_value(struct S s) { return s.a; }
// CHECK-LABEL: define dso_local i64 @by_value(i64 %s.coerce0, i64 %s.coerce1)
// CHECK-SAME:    [[ATTRS:#[0-9]+]]

long long __attribute__((ms_abi)) ms_by_value(struct S s) { return s.a; }
// CHECK-LABEL: define dso_local win64cc i64 @ms_by_value(ptr {{.*}}%s)

int variadic(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  int i = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return i;
}
// CHECK-LABEL: define dso_local i32 @variadic(i32 noundef %n, ...)
// CHECK:         %ap = alloca [1 x %struct.__va_list_tag], align 16

int __attribute__((ms_abi)) ms_variadic(int n, ...) {
  __builtin_ms_va_list ap;
  __builtin_ms_va_start(ap, n);
  int i = __builtin_va_arg(ap, int);
  __builtin_ms_va_end(ap);
  return i;
}
// CHECK-LABEL: define dso_local win64cc i32 @ms_variadic(i32 noundef %n, ...)
// CHECK:         %ap = alloca ptr, align 8

// A call without a prototype sets %al, as System V requires.
int no_proto();
int call_no_proto(void) { return no_proto(1.0); }
// CHECK-LABEL: define dso_local i32 @call_no_proto()
// CHECK:         call i32 (double, ...) @no_proto(double noundef 1.000000e+00)

int main(void) { return 0; }
// CHECK-LABEL: define dso_local i32 @main()

// CHECK: attributes [[ATTRS]] = { {{.*}}"no-stack-arg-probe"{{.*}}"stack-probe-size"="8192"

// CHECK: !llvm.linker.options = !{![[LIB:[0-9]+]], ![[MISMATCH:[0-9]+]]}
// CHECK: ![[LIB]] = !{!"/DEFAULTLIB:ntdll.lib"}
// CHECK: ![[MISMATCH]] = !{!"/FAILIFMISMATCH:\22key=value\22"}
