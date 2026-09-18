// REQUIRES: x86-registered-target

// Default libraries linked by the Windows Itanium toolchain: libc++ and
// libunwind from the toolchain, compiler-rt builtins, the UCRT and the
// Windows SDK import libraries. vcruntime and msvcrt are never linked.

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CXX_LIBS %s
// CXX_LIBS: lld-link
// CXX_LIBS-SAME: "c++.lib"
// CXX_LIBS-SAME: "unwind.lib"
// CXX_LIBS-SAME: "{{[^"]*}}clang_rt.builtins{{[^"]*}}.lib"
// CXX_LIBS-SAME: "ucrt.lib"
// CXX_LIBS-SAME: "kernel32.lib"
// CXX_LIBS-SAME: "ntdll.lib"
// CXX_LIBS-SAME: "-nodefaultlib:msvcrt"
// CXX_LIBS-SAME: "-nodefaultlib:msvcrtd"
// CXX_LIBS-SAME: "-nodefaultlib:vcruntime"
// CXX_LIBS-SAME: "-nodefaultlib:vcruntimed"
// CXX_LIBS-SAME: "-nodefaultlib:libcmt"
// CXX_LIBS-SAME: "-nodefaultlib:libcmtd"
// CXX_LIBS-SAME: "-nodefaultlib:oldnames"
// CXX_LIBS-SAME: "-nodefaultlib:ucrtd"
// CXX_LIBS-SAME: "-nodefaultlib:iso_stdio_wide_specifiers"
// CXX_LIBS-SAME: "-defaultlib:user32"
// CXX_LIBS-SAME: "-defaultlib:advapi32"
// CXX_LIBS-SAME: "-defaultlib:shell32"
// CXX_LIBS-NOT: "vcruntime.lib"
// CXX_LIBS-NOT: "msvcrt.lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck -check-prefix=C_LIBS %s
// C_LIBS: lld-link
// C_LIBS-SAME: "unwind.lib"
// C_LIBS-SAME: "ucrt.lib"
// C_LIBS-NOT: "c++.lib"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDLIB %s
// NOSTDLIB: lld-link
// NOSTDLIB-NOT: "c++.lib"
// NOSTDLIB-NOT: "unwind.lib"
// NOSTDLIB-NOT: "ucrt.lib"
// NOSTDLIB-NOT: "kernel32.lib"
// NOSTDLIB-NOT: "-nodefaultlib:"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NODEFAULTLIBS %s
// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-NOT: "c++.lib"
// NODEFAULTLIBS-NOT: "ucrt.lib"
// NODEFAULTLIBS-NOT: "-nodefaultlib:"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nolibc -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOLIBC %s
// NOLIBC: lld-link
// NOLIBC-SAME: "c++.lib"
// NOLIBC-NOT: "ucrt.lib"
// NOLIBC-NOT: "kernel32.lib"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -fexperimental-library \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=EXPERIMENTAL %s
// EXPERIMENTAL: "c++.lib"
// EXPERIMENTAL-SAME: "c++experimental.lib"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -stdlib=libc++ -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STDLIB_LIBCXX %s
// STDLIB_LIBCXX: "c++.lib"
// STDLIB_LIBCXX-NOT: "stdc++"
// STDLIB_LIBCXX-NOT: "msvcprt"

// compiler-rt is the only runtime library; asking for it explicitly changes
// nothing.
// RUN: %clang --target=x86_64-unknown-windows-itanium --rtlib=compiler-rt \
// RUN:   -### -x c %s 2>&1 | FileCheck -check-prefix=RTLIB_COMPILERRT %s
// RTLIB_COMPILERRT: "{{[^"]*}}clang_rt.builtins{{[^"]*}}.lib"
// RTLIB_COMPILERRT-SAME: "ucrt.lib"
// RTLIB_COMPILERRT-NOT: "vcruntime.lib"

// RUN: %clangxx --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBS_ARM64 %s
// LIBS_ARM64: "c++.lib"
// LIBS_ARM64-SAME: "unwind.lib"
// LIBS_ARM64-SAME: "ucrt.lib"
