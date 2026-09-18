// REQUIRES: x86-registered-target

// Threading on Windows Itanium is the UCRT's: no separate thread library,
// native TLS, and the MinGW-only -mthreads is ignored.

// RUN: %clang --target=x86_64-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PTHREAD %s
// PTHREAD: "-cc1"
// PTHREAD-NOT: "-lpthread"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fopenmp -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPENMP %s
// OPENMP: "-cc1"
// OPENMP-SAME: "-fopenmp"

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=TLS %s
// TLS: "-cc1"
// TLS-NOT: "-femulated-tls"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mthreads -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MTHREADS %s
// MTHREADS: warning: argument unused during compilation: '-mthreads'
// MTHREADS-NOT: error:
// RUN: %clang --target=x86_64-unknown-windows-itanium -mthreads -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MTHREADS_LINK %s
// MTHREADS_LINK: warning: argument unused during compilation: '-mthreads'
// MTHREADS_LINK: lld-link
// MTHREADS_LINK-NOT: mingwthrd

// RUN: %clang --target=aarch64-unknown-windows-itanium -pthread -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_THREAD %s
// ARM64_THREAD: "-cc1"
// ARM64_THREAD-NOT: "-lpthread"
