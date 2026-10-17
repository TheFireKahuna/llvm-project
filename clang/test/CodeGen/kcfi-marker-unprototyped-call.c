// RUN: %clang_cc1 -std=c17 -Wno-deprecated-non-prototype -fsanitize=kcfi -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -std=c17 -Wno-deprecated-non-prototype -fsanitize=kcfi -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

/// A call through a pointer without a prototype checks the type of its
/// promoted arguments. The pointer may hold any function, a foreign one too,
/// so the call opens that type as a cast to it would.

typedef int (*noproto)();

// CHECK: define {{.*}} @w_int({{.*}} !kcfi_type ![[#INT:]]
int w_int(int x) { return x; }

int call(noproto p) { return p(1); }

// CHECK: !kcfi.dynamic = !{![[#INT]]}
