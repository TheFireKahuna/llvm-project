// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -fno-function-type-prefix -fno-auto-import \
// RUN:   -DNO_ITANIUM | FileCheck --check-prefix=PLT %s \
// RUN:   --implicit-check-not=dllimport
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -fno-plt -fcxx-exceptions \
// RUN:   -fexceptions -emit-llvm -o - %s | FileCheck --check-prefix=PLT %s \
// RUN:   --implicit-check-not=dllimport
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -fno-function-type-prefix -fno-auto-import \
