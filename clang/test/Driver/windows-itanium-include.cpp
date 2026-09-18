// REQUIRES: x86-registered-target

// Test Windows Itanium toolchain include path handling.

// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SYSROOT-INCLUDES %s

// SYSROOT-INCLUDES: "-internal-isystem" "{{.*}}windows_itanium_tree{{.*}}c++{{.*}}v1"

// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdinc -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDINC %s

// NOSTDINC-NOT: "-internal-isystem"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -nostdinc++ -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDINCXX %s

// NOSTDINCXX-NOT: "{{.*}}c++{{.*}}v1"

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=RESOURCE-DIR %s

// RESOURCE-DIR: "-resource-dir" "{{.*}}clang{{.*}}"

// RUN: %clang --target=x86_64-unknown-windows-itanium -isystem /custom/include -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=ISYSTEM %s

// ISYSTEM: "-isystem" "/custom/include"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /imsvc "C:/SDK/include" /c -### -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=IMSVC %s

// IMSVC: "-cc1"
// IMSVC: "-internal-isystem" "C:/SDK/include"
