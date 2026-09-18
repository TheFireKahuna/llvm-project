// REQUIRES: x86-registered-target

// Runtime library selection for Windows Itanium: compiler-rt builtins,
// wincrt, and the UCRT; never vcruntime or msvcrt.

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFAULT_RUNTIME %s
// DEFAULT_RUNTIME: lld-link
// DEFAULT_RUNTIME-SAME: "unwind.lib"
// DEFAULT_RUNTIME-SAME: "{{[^"]*}}clang_rt.builtins{{[^"]*}}.lib"
// DEFAULT_RUNTIME-SAME: "ucrt.lib"
// DEFAULT_RUNTIME-SAME: "kernel32.lib"
// DEFAULT_RUNTIME-SAME: "ntdll.lib"
// DEFAULT_RUNTIME-SAME: "-nodefaultlib:msvcrt"
// DEFAULT_RUNTIME-SAME: "-nodefaultlib:vcruntime"
// DEFAULT_RUNTIME-NOT: "vcruntime.lib"
// DEFAULT_RUNTIME-NOT: "msvcrt.lib"

// --rtlib=compiler-rt is the only accepted runtime library.
// RUN: %clang --target=x86_64-unknown-windows-itanium --rtlib=compiler-rt -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFAULT_RUNTIME %s
// RUN: not %clang --target=x86_64-unknown-windows-itanium --rtlib=msvcrt -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=RTLIB_MSVCRT %s
// RTLIB_MSVCRT: error: invalid runtime library name in argument '--rtlib=msvcrt'

// The clang-cl runtime selectors only add directives; the linker blocks the
// MSVC runtime libraries they name.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /MD -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_MD %s
// CL_MD: "-cc1"
// CL_MD-SAME: "--dependent-lib=msvcrt"
// CL_MD: lld-link
// CL_MD-SAME: "-nodefaultlib:msvcrt"

// Multi-architecture.
// RUN: %clang --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_RUNTIME %s
// ARM64_RUNTIME: lld-link
// ARM64_RUNTIME-SAME: "-machine:arm64"
// ARM64_RUNTIME-SAME: "{{[^"]*}}clang_rt.builtins{{[^"]*}}.lib"
// ARM64_RUNTIME-SAME: "ucrt.lib"
// ARM64_RUNTIME-SAME: "-nodefaultlib:msvcrt"
