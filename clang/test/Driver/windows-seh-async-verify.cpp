// RUN: %clang --target=x86_64-pc-windows -fasync-exceptions -fsyntax-only -### %s 2>&1 | FileCheck %s
// RUN: %clang_cl --target=x86_64-pc-windows /EHa -fsyntax-only -### -- %s 2>&1 | FileCheck %s
// RUN: %clang --target=x86_64-pc-windows-gnu -fasync-exceptions -fsyntax-only -### %s 2>&1 | FileCheck %s --check-prefixes=GNU-ALL,GNU
// RUN: %clang_cl --target=x86_64-pc-windows-gnu /EHa -fsyntax-only -### -- %s 2>&1 | FileCheck %s --check-prefixes=GNU-ALL,CL-GNU
// RUN: not %clang --target=x86_64-unknown-windows-itanium -fasync-exceptions -fsyntax-only -### %s 2>&1 | FileCheck %s --check-prefixes=GNU-ALL,ITANIUM
// RUN: not %clang_cl --target=aarch64-unknown-windows-itanium /EHa -fsyntax-only -### -- %s 2>&1 | FileCheck %s --check-prefixes=GNU-ALL,CL-ITANIUM
// RUN: not %clang --target=aarch64-pc-windows-ntposix -fasync-exceptions -fsyntax-only -### %s 2>&1 | FileCheck %s --check-prefixes=GNU-ALL,NTPOSIX
// RUN: not %clang_cl --target=x86_64-pc-windows-ntposix /EHa -fsyntax-only -### -- %s 2>&1 | FileCheck %s --check-prefixes=GNU-ALL,CL-NTPOSIX

// CHECK-NOT: warning
// GNU: warning: argument unused during compilation: '-fasync-exceptions' [-Wunused-command-line-argument]
// CL-GNU: warning: argument unused during compilation: '/EHa' [-Wunused-command-line-argument]
// ITANIUM: error: unsupported option '-fasync-exceptions' for target 'x86_64-unknown-windows-itanium'
// CL-ITANIUM: error: unsupported option '/EHa' for target 'aarch64-unknown-windows-itanium'
// NTPOSIX: error: unsupported option '-fasync-exceptions' for target 'aarch64-pc-windows-ntposix'
// CL-NTPOSIX: error: unsupported option '/EHa' for target 'x86_64-pc-windows-ntposix'

// CHECK: -fasync-exceptions
// GNU-ALL-NOT: -fasync-exceptions
struct S {
    union _Un {
        ~_Un() {}
        char _Buf[12];
    };
    _Un _un;
};

struct Embed {
    S v2;
};

void PR62449() { Embed v{}; }
