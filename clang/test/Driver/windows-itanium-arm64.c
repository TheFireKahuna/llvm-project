// REQUIRES: aarch64-registered-target

// Test ARM64 architecture support for Windows Itanium toolchain.

// RUN: %clang --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64 %s

// ARM64: "-cc1"
// ARM64-SAME: "-triple" "aarch64-unknown-windows-itanium"
// ARM64: lld-link
// ARM64-SAME: "-machine:arm64"

// RUN: %clang --target=arm64ec-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64EC %s

// ARM64EC: "-cc1"
// ARM64EC-SAME: "-triple" "arm64ec-unknown-windows-itanium"
// ARM64EC: lld-link
// ARM64EC-SAME: "-machine:arm64ec"

// RUN: %clang --target=aarch64-unknown-windows-itanium -marm64x -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64X %s

// ARM64X: lld-link
// ARM64X-SAME: "-machine:arm64x"

// RUN: %clang --target=aarch64-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_DLL %s

// ARM64_DLL: lld-link
// ARM64_DLL-SAME: "-dll"
// ARM64_DLL-SAME: "-entry:_DllMainCRTStartup"
// ARM64_DLL-SAME: "-machine:arm64"

// RUN: %clang --target=arm64ec-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64EC_DLL %s

// ARM64EC_DLL: lld-link
// ARM64EC_DLL-SAME: "-dll"
// ARM64EC_DLL-SAME: "-machine:arm64ec"

// RUN: %clang --target=aarch64-unknown-windows-itanium -fsanitize=address -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_ASAN %s

// ARM64_ASAN: "-cc1"
// ARM64_ASAN-SAME: "-fsanitize=address"
// ARM64_ASAN: lld-link
// ARM64_ASAN-SAME: "-machine:arm64"

// RUN: %clang --target=aarch64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_EH %s

// ARM64_EH: "-cc1"
// ARM64_EH-SAME: "-exception-model=seh"

// RUN: %clang --target=aarch64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_PIC %s

// ARM64_PIC: "-cc1"
// ARM64_PIC-SAME: "-mrelocation-model" "pic"

// RUN: %clang --target=aarch64-unknown-windows-itanium -fveclib=ArmPL -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_VECLIB %s

// ARM64_VECLIB: lld-link
// ARM64_VECLIB: "--dependent-lib=amath"
