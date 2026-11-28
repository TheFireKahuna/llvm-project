// RUN: %clang -### -c --target=aarch64 -fno-plt -Werror %s 2>&1 | FileCheck %s --check-prefix=NOPLT
// RUN: %clang -### -c --target=x86_64 -fno-plt -Werror %s 2>&1 | FileCheck %s --check-prefix=NOPLT

// RUN: %clang -### -c --target=aarch64 -fno-plt -fplt -Werror %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -### -c --target=powerpc64 -fno-plt %s 2>&1 | FileCheck %s --check-prefixes=WARN,DEFAULT
// RUN: %clang -### -c --target=aarch64-windows -fno-plt %s 2>&1 | FileCheck %s --check-prefixes=WARN,DEFAULT

// On Windows Itanium and NT-POSIX the PLT entry is the linker's thunk for an
// imported function, which they avoid by default.
// RUN: %clang -### -c --target=x86_64-unknown-windows-itanium -Werror %s 2>&1 | FileCheck %s --check-prefix=NOPLT
// RUN: %clang -### -c --target=x86_64-pc-windows-ntposix -fplt -Werror %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -### -c --target=aarch64-unknown-windows-itanium -Werror %s 2>&1 | FileCheck %s --check-prefix=NOPLT
// RUN: %clang -### -c --target=aarch64-pc-windows-ntposix -fplt -Werror %s 2>&1 | FileCheck %s --check-prefix=DEFAULT

// WARN: warning: argument unused during compilation: '-fno-plt' [-Wunused-command-line-argument]
// NOPLT: "-fno-plt"
// DEFAULT-NOT: "-fno-plt"
