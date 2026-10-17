// Windows Itanium and NT-POSIX check the layouts of C structs and the types of
// extern "C" data between images only under -fabi-witness-c-types, which the
// drivers pass to the compiler; other targets have no such checks.

// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEFAULT %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:   -fabi-witness-c-types 2>&1 | FileCheck --check-prefix=CTYPES %s
// RUN: %clang -### --target=aarch64-pc-windows-ntposix -c %s \
// RUN:   -fabi-witness-c-types 2>&1 | FileCheck --check-prefix=CTYPES %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:   -fabi-witness-c-types -fno-abi-witness-c-types 2>&1 \
// RUN:   | FileCheck --check-prefix=DEFAULT %s
// RUN: not %clang -### --target=x86_64-unknown-linux-gnu -c %s \
// RUN:   -fabi-witness-c-types 2>&1 | FileCheck --check-prefix=OTHER %s

// DEFAULT-NOT: "-fabi-witness-c-types"
// CTYPES: "-fabi-witness-c-types"
// OTHER: error: unsupported option '-fabi-witness-c-types' for target 'x86_64-unknown-linux-gnu'
