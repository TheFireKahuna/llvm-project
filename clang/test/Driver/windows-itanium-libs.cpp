// REQUIRES: x86-registered-target
// REQUIRES: win32-itanium-default-libc-system

// Test default library linking for Windows Itanium toolchain.

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CXX_LIBS %s

// CXX_LIBS: "-defaultlib:c++"
// CXX_LIBS-SAME: "-defaultlib:unwind"
// CXX_LIBS-SAME: "-defaultlib:ucrt"
// CXX_LIBS-SAME: "-defaultlib:msvcrt"
// CXX_LIBS-SAME: "-defaultlib:oldnames"
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

// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck -check-prefix=C_LIBS %s

// C_LIBS: "-defaultlib:unwind"
// C_LIBS-SAME: "-defaultlib:ucrt"
// C_LIBS-SAME: "-defaultlib:msvcrt"
// C_LIBS-NOT: "-defaultlib:c++"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDLIB %s

// NOSTDLIB: lld-link
// NOSTDLIB-NOT: "-defaultlib:c++"
// NOSTDLIB-NOT: "-defaultlib:unwind"
// NOSTDLIB-NOT: "-defaultlib:ucrt"
// NOSTDLIB-NOT: "-defaultlib:msvcrt"
// NOSTDLIB-NOT: "-defaultlib:kernel32"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NODEFAULTLIBS %s

// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-NOT: "-defaultlib:c++"
// NODEFAULTLIBS-NOT: "-defaultlib:ucrt"
// NODEFAULTLIBS-NOT: "-defaultlib:msvcrt"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -fexperimental-library \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=EXPERIMENTAL %s

// EXPERIMENTAL: "-defaultlib:c++"
// EXPERIMENTAL-SAME: "-defaultlib:c++experimental"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -stdlib=libc++ -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STDLIB_LIBCXX %s

// STDLIB_LIBCXX: "-defaultlib:c++"
// STDLIB_LIBCXX-NOT: "-defaultlib:stdc++"
// STDLIB_LIBCXX-NOT: "-defaultlib:msvcprt"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=WIN_API_LIBS %s

// WIN_API_LIBS: "-defaultlib:winspool"
// WIN_API_LIBS-SAME: "-defaultlib:comdlg32"
// WIN_API_LIBS-SAME: "-defaultlib:odbc32"
// WIN_API_LIBS-SAME: "-defaultlib:odbccp32"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fopenmp -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPENMP %s

// OPENMP: lld-link
// OPENMP-SAME: "-nodefaultlib:vcomp.lib"
// OPENMP-SAME: "-nodefaultlib:vcompd.lib"
// OPENMP-SAME: "-defaultlib:libomp.lib"

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

// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck -check-prefix=RTLIB_DEFAULT %s

// RTLIB_DEFAULT: "-defaultlib:ucrt"
// RTLIB_DEFAULT-SAME: "-defaultlib:msvcrt"

// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -### -x c %s 2>&1 | FileCheck -check-prefix=RTLIB_COMPILERRT %s

// RTLIB_COMPILERRT: "-defaultlib:ucrt"
// RTLIB_COMPILERRT-NOT: "-defaultlib:msvcrt"
// RTLIB_COMPILERRT: "clang_rt.builtins-x86_64.lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=platform \
// RUN:   -### -x c %s 2>&1 | FileCheck -check-prefix=RTLIB_PLATFORM %s

// RTLIB_PLATFORM: "-defaultlib:ucrt"
// RTLIB_PLATFORM-SAME: "-defaultlib:msvcrt"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=CXX_COMPILERRT %s

// CXX_COMPILERRT: "-defaultlib:c++"
// CXX_COMPILERRT: "-defaultlib:unwind"
// CXX_COMPILERRT: "-defaultlib:ucrt"
// CXX_COMPILERRT-NOT: "-defaultlib:msvcrt"
// CXX_COMPILERRT: "clang_rt.builtins-x86_64.lib"
