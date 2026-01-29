// REQUIRES: x86-registered-target

// Test Windows Itanium toolchain C++ include path handling.
// The toolchain searches for libc++ headers in:
// 1. Target-specific adjacent to clang: <install>/include/<target>/c++/v1
// 2. Adjacent to clang: <install>/include/c++/v1
// 3. In library paths
// 4. In sysroot: <sysroot>/include/c++/v1

//===----------------------------------------------------------------------===//
// Test libc++ headers found via sysroot
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SYSROOT-INCLUDES %s

// SYSROOT-INCLUDES: "-internal-isystem" "{{.*}}windows_itanium_tree{{.*}}c++{{.*}}v1"

//===----------------------------------------------------------------------===//
// Test -nostdinc suppresses system includes
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdinc -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDINC %s

// NOSTDINC-NOT: "-internal-isystem"

//===----------------------------------------------------------------------===//
// Test -nostdinc++ suppresses C++ includes but keeps system includes
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -nostdinc++ -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDINCXX %s

// NOSTDINCXX-NOT: "{{.*}}c++{{.*}}v1"

//===----------------------------------------------------------------------===//
// Test clang resource directory is set
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=RESOURCE-DIR %s

// RESOURCE-DIR: "-resource-dir" "{{.*}}clang{{.*}}"

//===----------------------------------------------------------------------===//
// Test -isystem for adding custom include paths
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -isystem /custom/include -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=ISYSTEM %s

// ISYSTEM: "-isystem" "/custom/include"

//===----------------------------------------------------------------------===//
// Test /imsvc (MSVC-style system include) via clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /imsvc "C:/SDK/include" /c -### -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=IMSVC %s

// IMSVC: "-cc1"
// IMSVC: "-internal-isystem" "C:/SDK/include"
