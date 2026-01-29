// REQUIRES: x86-registered-target

// Test threading support for Windows Itanium toolchain.
// Windows Itanium uses Win32 thread APIs via the MSVC runtime libraries.

//===----------------------------------------------------------------------===//
// -pthread is accepted but not required on Windows
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PTHREAD %s

// -pthread is accepted on Windows but has no special effect since threading
// is built into the runtime libraries. No -lpthread is needed.
// PTHREAD: "-cc1"
// PTHREAD-NOT: "-lpthread"

//===----------------------------------------------------------------------===//
// OpenMP threading (-fopenmp)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fopenmp -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPENMP %s

// OpenMP uses libomp instead of MSVC's vcomp
// OPENMP: lld-link
// OPENMP-SAME: "-nodefaultlib:vcomp.lib"
// OPENMP-SAME: "-nodefaultlib:vcompd.lib"
// OPENMP: "-defaultlib:libomp.lib"

//===----------------------------------------------------------------------===//
// Thread-local storage (TLS)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=TLS %s

// Windows uses native TLS, not emulated
// TLS: "-cc1"
// TLS-NOT: "-femulated-tls"

//===----------------------------------------------------------------------===//
// -mthreads flag is ignored on Windows Itanium (with warning)
//===----------------------------------------------------------------------===//

// -mthreads is a MinGW-specific flag that links mingwthrd. On Windows Itanium,
// threading is handled by the MSVC runtime which is already thread-safe, so
// the flag is ignored with a warning (same as MinGW with -c compile-only).

// RUN: %clang --target=x86_64-unknown-windows-itanium -mthreads -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MTHREADS %s

// MTHREADS: warning: argument unused during compilation: '-mthreads'
// MTHREADS-NOT: error:

// RUN: %clang --target=x86_64-unknown-windows-itanium -mthreads -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MTHREADS_LINK %s

// Full compilation: flag is ignored, no mingwthrd linked
// MTHREADS_LINK: warning: argument unused during compilation: '-mthreads'
// MTHREADS_LINK: lld-link
// MTHREADS_LINK-NOT: mingwthrd

//===----------------------------------------------------------------------===//
// Threading on different architectures
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_THREAD %s

// X86_THREAD: "-cc1"
// X86_THREAD-NOT: "-lpthread"

// RUN: %clang --target=aarch64-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_THREAD %s

// ARM64_THREAD: "-cc1"
// ARM64_THREAD-NOT: "-lpthread"

//===----------------------------------------------------------------------===//
// C11 threads (_Thread_local support)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -std=c11 -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=C11_THREADS %s

// C11 _Thread_local uses native TLS
// C11_THREADS: "-cc1"
// C11_THREADS-SAME: "-std=c11"
