// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-sanitize=kcfi -S \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang --target=aarch64-unknown-windows-itanium -fno-sanitize=kcfi -S \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang --target=x86_64-pc-windows-ntposix -fno-sanitize=kcfi -S \
// RUN:     -emit-llvm -o - %s | FileCheck %s

// -fno-sanitize=kcfi removes the checks, but every function keeps its type
// prefix and the module its marker, since checks in other images read them.

// CHECK:     define {{.*}}void @f() {{.*}}!kcfi_type
// CHECK-NOT: !"kcfi"
// CHECK:     !{i32 4, !"function-type-prefix", i32 {{-?[0-9]+}}}
// CHECK-NOT: !"kcfi"

void f(void) {}
