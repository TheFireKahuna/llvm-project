// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -DWEAK_ONLY -emit-llvm -o - %s | FileCheck --check-prefix=NONE %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm -o - %s | FileCheck --check-prefix=NONE %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm -o - %s | FileCheck --check-prefix=NONE %s

// A strong definition of a replaceable function replaces the runtime's weak
// default for every image, which only the import-slots binding carries out by
// writing the override table. The module references a marker that this binding
// alone defines, so a link that cannot carry it out, such as one by another
// linker, fails with an unresolved symbol rather than leaving the program with
// a split heap. The runtime's own weak default is the fallback, not a
// replacement, and references nothing.

typedef decltype(sizeof(0)) size_t;
extern "C" void *malloc(size_t);

#ifdef WEAK_ONLY
[[gnu::weak]] void *operator new(size_t n) { return malloc(n); }
#else
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
#endif

// CHECK: !{!"/INCLUDE:__llvm_overrides_v1"}

// NONE-NOT: __llvm_overrides_v1
