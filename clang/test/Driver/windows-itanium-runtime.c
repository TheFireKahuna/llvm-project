// REQUIRES: x86-registered-target
// REQUIRES: win32-itanium-default-libc-system

// Test runtime library selection for Windows Itanium.

// Default: dynamic CRT.
// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFAULT_RUNTIME %s
// DEFAULT_RUNTIME: "-defaultlib:msvcrt"
// DEFAULT_RUNTIME-SAME: "-defaultlib:ucrt"

// clang_cl CRT selection.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MD /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MD %s
// CL_MD: "-cc1"
// CL_MD-SAME: "--dependent-lib=msvcrt"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MDd /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MDD %s
// CL_MDD: "-cc1"
// CL_MDD-SAME: "--dependent-lib=msvcrtd"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MT /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MT %s
// CL_MT: "-cc1"
// CL_MT-SAME: "--dependent-lib=libcmt"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MTd /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MTD %s
// CL_MTD: "-cc1"
// CL_MTD-SAME: "--dependent-lib=libcmtd"

// Multi-architecture.
// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_RUNTIME %s
// X86_RUNTIME: "-defaultlib:msvcrt"

// RUN: %clang --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_RUNTIME %s
// ARM64_RUNTIME: "-defaultlib:msvcrt"

// Unwind library for exception support.
// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=UNWIND_LIB %s
// UNWIND_LIB: "-defaultlib:unwind"
