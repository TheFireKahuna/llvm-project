// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -O2 -fsemantic-interposition -emit-llvm -o - %s | FileCheck --check-prefix=PREEMPT %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -O2 -fsemantic-interposition -emit-llvm -o - %s | FileCheck --check-prefix=PREEMPT %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -mdefault-visibility-export-mapping=explicit -O2 -fsemantic-interposition -emit-llvm -o - %s | FileCheck --check-prefix=PREEMPT %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -O2 -emit-llvm -o - %s | FileCheck --check-prefix=LOCAL %s

// Under -fsemantic-interposition a function with default visibility may be
// preempted by another image's definition in its own image, as on ELF: it is
// not DSO local, so the optimizer neither inlines a call to it nor binds the
// call locally, and the linker redirects its references. A hidden or static
// function stays local. Without the option, the function is DSO local and its
// calls in its own unit may be inlined.

// PREEMPT-LABEL: define dllexport i32 @f()
// LOCAL-LABEL:   define dso_local dllexport {{.*}}i32 @f()
__attribute__((visibility("default"))) int f(void) { return 1; }

// PREEMPT-LABEL: define hidden {{.*}}i32 @h()
__attribute__((visibility("hidden"))) int h(void) { return 2; }

// PREEMPT-LABEL: define dllexport i32 @g()
// PREEMPT:         call i32 @f()
// LOCAL-LABEL:   define dso_local dllexport {{.*}}i32 @g()
// LOCAL-NOT:       call
// LOCAL:           ret i32 1
__attribute__((visibility("default"))) int g(void) { return f(); }

// PREEMPT: !{{[0-9]+}} = !{i32 1, !"SemanticInterposition", i32 1}
