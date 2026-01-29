// REQUIRES: x86-registered-target

// Test default library linking for Windows Itanium toolchain.
// This verifies the correct runtime libraries are linked.

//===----------------------------------------------------------------------===//
// C++ compilation: all default libraries
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CXX_LIBS %s

// C++ standard library
// CXX_LIBS: "-defaultlib:c++"

// Unwinding library
// CXX_LIBS-SAME: "-defaultlib:unwind"

// Universal C Runtime (C library functions)
// CXX_LIBS-SAME: "-defaultlib:ucrt"

// MS Visual C Runtime (entry points, default when not using compiler-rt)
// CXX_LIBS-SAME: "-defaultlib:msvcrt"

// Legacy stdio for _NO_CRT_STDIO_INLINE
// CXX_LIBS-SAME: "-defaultlib:legacy_stdio_definitions"

// POSIX compatibility
// CXX_LIBS-SAME: "-defaultlib:oldnames"

// Windows API libraries (same list as Visual Studio's CoreLibraryDependencies)
// CXX_LIBS: "-defaultlib:kernel32"
// CXX_LIBS: "-defaultlib:user32"
// CXX_LIBS: "-defaultlib:gdi32"
// CXX_LIBS: "-defaultlib:winspool"
// CXX_LIBS: "-defaultlib:comdlg32"
// CXX_LIBS: "-defaultlib:advapi32"
// CXX_LIBS: "-defaultlib:shell32"
// CXX_LIBS: "-defaultlib:ole32"
// CXX_LIBS: "-defaultlib:oleaut32"
// CXX_LIBS: "-defaultlib:uuid"
// CXX_LIBS: "-defaultlib:odbc32"
// CXX_LIBS: "-defaultlib:odbccp32"

//===----------------------------------------------------------------------===//
// C compilation: no C++ library
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck -check-prefix=C_LIBS %s

// C_LIBS: "-defaultlib:unwind"
// C_LIBS-SAME: "-defaultlib:ucrt"
// C_LIBS-SAME: "-defaultlib:msvcrt"
// C_LIBS-NOT: "-defaultlib:c++"

//===----------------------------------------------------------------------===//
// -nostdlib suppresses all libraries
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDLIB %s

// NOSTDLIB: lld-link
// NOSTDLIB-NOT: "-defaultlib:c++"
// NOSTDLIB-NOT: "-defaultlib:unwind"
// NOSTDLIB-NOT: "-defaultlib:ucrt"
// NOSTDLIB-NOT: "-defaultlib:msvcrt"
// NOSTDLIB-NOT: "-defaultlib:kernel32"

//===----------------------------------------------------------------------===//
// -nodefaultlibs suppresses default libraries
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NODEFAULTLIBS %s

// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-NOT: "-defaultlib:c++"
// NODEFAULTLIBS-NOT: "-defaultlib:ucrt"
// NODEFAULTLIBS-NOT: "-defaultlib:msvcrt"

//===----------------------------------------------------------------------===//
// -fexperimental-library adds c++experimental
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -fexperimental-library \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=EXPERIMENTAL %s

// EXPERIMENTAL: "-defaultlib:c++"
// EXPERIMENTAL-SAME: "-defaultlib:c++experimental"

//===----------------------------------------------------------------------===//
// -stdlib=libc++ is the only supported option (default)
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -stdlib=libc++ -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STDLIB_LIBCXX %s

// STDLIB_LIBCXX: "-defaultlib:c++"
// STDLIB_LIBCXX-NOT: "-defaultlib:stdc++"
// STDLIB_LIBCXX-NOT: "-defaultlib:msvcprt"

//===----------------------------------------------------------------------===//
// Additional Windows API libraries (all linked by default)
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=WIN_API_LIBS %s

// WIN_API_LIBS: "-defaultlib:winspool"
// WIN_API_LIBS-SAME: "-defaultlib:comdlg32"
// WIN_API_LIBS-SAME: "-defaultlib:odbc32"
// WIN_API_LIBS-SAME: "-defaultlib:odbccp32"

//===----------------------------------------------------------------------===//
// OpenMP support (uses libomp instead of vcomp)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fopenmp -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPENMP %s

// OPENMP: lld-link
// OPENMP-SAME: "-nodefaultlib:vcomp.lib"
// OPENMP-SAME: "-nodefaultlib:vcompd.lib"
// OPENMP-SAME: "-defaultlib:libomp.lib"

//===----------------------------------------------------------------------===//
// Libraries on different architectures
//===----------------------------------------------------------------------===//

// RUN: %clangxx --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBS_X86 %s

// LIBS_X86: "-defaultlib:c++"
// LIBS_X86-SAME: "-defaultlib:ucrt"
// LIBS_X86-SAME: "-defaultlib:msvcrt"

// RUN: %clangxx --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBS_ARM64 %s

// LIBS_ARM64: "-defaultlib:c++"
// LIBS_ARM64-SAME: "-defaultlib:ucrt"
// LIBS_ARM64-SAME: "-defaultlib:msvcrt"

//===----------------------------------------------------------------------===//
// Runtime library selection via -rtlib flag
//===----------------------------------------------------------------------===//

// Default is platform: links both ucrt and msvcrt
// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck -check-prefix=RTLIB_DEFAULT %s

// RTLIB_DEFAULT: "-defaultlib:ucrt"
// RTLIB_DEFAULT-SAME: "-defaultlib:msvcrt"

// Explicit -rtlib=compiler-rt: uses ucrt only (entry points from compiler-rt)
// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -### -x c %s 2>&1 | FileCheck -check-prefix=RTLIB_COMPILERRT %s

// RTLIB_COMPILERRT: "-defaultlib:ucrt"
// RTLIB_COMPILERRT-NOT: "-defaultlib:msvcrt"

// -rtlib=platform: same as default (ucrt + msvcrt)
// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=platform \
// RUN:   -### -x c %s 2>&1 | FileCheck -check-prefix=RTLIB_PLATFORM %s

// RTLIB_PLATFORM: "-defaultlib:ucrt"
// RTLIB_PLATFORM-SAME: "-defaultlib:msvcrt"
