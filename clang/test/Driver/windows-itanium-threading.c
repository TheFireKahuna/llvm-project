// REQUIRES: x86-registered-target

// Test threading for Windows Itanium. Uses Win32 APIs via MSVC runtime.

// -pthread accepted but has no effect (threading built into runtime).
// RUN: %clang --target=x86_64-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PTHREAD %s
// PTHREAD: "-cc1"
// PTHREAD-NOT: "-lpthread"

// OpenMP uses libomp.
// RUN: %clang --target=x86_64-unknown-windows-itanium -fopenmp -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPENMP %s
// OPENMP: lld-link
// OPENMP-SAME: "-nodefaultlib:vcomp.lib"
// OPENMP-SAME: "-nodefaultlib:vcompd.lib"
// OPENMP: "-defaultlib:libomp.lib"

// Native TLS, not emulated.
// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=TLS %s
// TLS: "-cc1"
// TLS-NOT: "-femulated-tls"

// -mthreads is MinGW-specific; ignored on Windows Itanium.
// RUN: %clang --target=x86_64-unknown-windows-itanium -mthreads -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MTHREADS %s
// MTHREADS: warning: argument unused during compilation: '-mthreads'
// MTHREADS-NOT: error:

// RUN: %clang --target=x86_64-unknown-windows-itanium -mthreads -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MTHREADS_LINK %s
// MTHREADS_LINK: warning: argument unused during compilation: '-mthreads'
// MTHREADS_LINK: lld-link
// MTHREADS_LINK-NOT: mingwthrd

// Multi-architecture.
// RUN: %clang --target=i686-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_THREAD %s
// X86_THREAD: "-cc1"
// X86_THREAD-NOT: "-lpthread"

// RUN: %clang --target=aarch64-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_THREAD %s
// ARM64_THREAD: "-cc1"
// ARM64_THREAD-NOT: "-lpthread"

// C11 _Thread_local uses native TLS.
// RUN: %clang --target=x86_64-unknown-windows-itanium -std=c11 -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=C11_THREADS %s
// C11_THREADS: "-cc1"
// C11_THREADS-SAME: "-std=c11"
