// REQUIRES: x86-registered-target

// Test runtime library selection for Windows Itanium toolchain.
// Windows Itanium uses MSVC-compatible runtimes with the Itanium ABI.

//===----------------------------------------------------------------------===//
// Default runtime: dynamic msvcrt (via clang driver)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFAULT_RUNTIME %s

// Default links against dynamic CRT (msvcrt) not static (libcmt)
// DEFAULT_RUNTIME: "-defaultlib:msvcrt"
// DEFAULT_RUNTIME-SAME: "-defaultlib:ucrt"

//===----------------------------------------------------------------------===//
// /MD (dynamic release) via clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MD /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MD %s

// CL_MD: "-cc1"
// CL_MD-SAME: "--dependent-lib=msvcrt"

//===----------------------------------------------------------------------===//
// /MDd (dynamic debug) via clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MDd /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MDD %s

// CL_MDD: "-cc1"
// CL_MDD-SAME: "--dependent-lib=msvcrtd"

//===----------------------------------------------------------------------===//
// /MT (static release) via clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MT /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MT %s

// CL_MT: "-cc1"
// CL_MT-SAME: "--dependent-lib=libcmt"

//===----------------------------------------------------------------------===//
// /MTd (static debug) via clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MTd /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MTD %s

// CL_MTD: "-cc1"
// CL_MTD-SAME: "--dependent-lib=libcmtd"

//===----------------------------------------------------------------------===//
// Runtime on different architectures
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_RUNTIME %s

// X86_RUNTIME: "-defaultlib:msvcrt"

// RUN: %clang --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_RUNTIME %s

// ARM64_RUNTIME: "-defaultlib:msvcrt"

//===----------------------------------------------------------------------===//
// Exception handling requires unwind library
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=UNWIND_LIB %s

// Unwind library is always linked for exception support
// UNWIND_LIB: "-defaultlib:unwind"

//===----------------------------------------------------------------------===//
// Legacy stdio definitions for _NO_CRT_STDIO_INLINE
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LEGACY_STDIO %s

// Must link legacy_stdio_definitions for _NO_CRT_STDIO_INLINE to work
// LEGACY_STDIO: "-defaultlib:legacy_stdio_definitions"
