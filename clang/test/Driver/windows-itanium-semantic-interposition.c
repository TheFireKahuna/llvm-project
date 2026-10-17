// On Windows Itanium and NT-POSIX, -fsemantic-interposition reaches the
// compiler, as on ELF; without it, or with -fno-semantic-interposition, no
// interposition option does.

// RUN: %clang --target=x86_64-unknown-windows-itanium -fsemantic-interposition -c -### %s 2>&1 | FileCheck %s
// RUN: %clang --target=aarch64-unknown-windows-itanium -fsemantic-interposition -c -### %s 2>&1 | FileCheck %s
// RUN: %clang --target=x86_64-pc-windows-ntposix -fsemantic-interposition -c -### %s 2>&1 | FileCheck %s
// CHECK: "-fsemantic-interposition"

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 | FileCheck --check-prefix=NONE %s
// RUN: %clang --target=x86_64-unknown-windows-itanium -fsemantic-interposition -fno-semantic-interposition -c -### %s 2>&1 | FileCheck --check-prefix=NONE %s
// NONE-NOT: "-fsemantic-interposition"
// NONE-NOT: "-fhalf-no-semantic-interposition"
