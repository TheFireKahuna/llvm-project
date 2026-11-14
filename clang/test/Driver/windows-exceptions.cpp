// RUN: %clang -target i686-windows-msvc -c %s -### 2>&1 | FileCheck -check-prefix=MSVC -check-prefix=MSVC-NOSEH %s
// RUN: %clang -target x86_64-windows-msvc -c %s -### 2>&1 | FileCheck -check-prefix=MSVC -check-prefix=MSVC-SEH %s
// RUN: %clang -target armv7-windows-msvc -c %s -### 2>&1 | FileCheck -check-prefix=MSVC -check-prefix=MSVC-SEH %s
// RUN: %clang -target aarch64-windows-msvc -c %s -### 2>&1 | FileCheck -check-prefix=MSVC -check-prefix=MSVC-SEH %s
// RUN: %clang -target i686-windows-gnu -c %s -### 2>&1 | FileCheck -check-prefix=MINGW-DWARF %s
// RUN: %clang -target x86_64-windows-gnu -c %s -### 2>&1 | FileCheck -check-prefix=MINGW-SEH %s
// RUN: %clang -target armv7-windows-gnu -fdwarf-exceptions -c %s -### 2>&1 | FileCheck -check-prefix=MINGW-DWARF %s
// RUN: %clang -target armv7-windows-gnu -c %s -### 2>&1 | FileCheck -check-prefix=MINGW-SEH %s
// RUN: %clang -target aarch64-windows-gnu -fdwarf-exceptions -c %s -### 2>&1 | FileCheck -check-prefix=MINGW-DWARF %s
// RUN: %clang -target aarch64-windows-gnu -c %s -### 2>&1 | FileCheck -check-prefix=MINGW-SEH %s
// RUN: %clang --target=x86_64-windows-itanium -fsjlj-exceptions -c %s -### 2>&1 | FileCheck -check-prefixes=ITANIUM,ITANIUM-SJLJ %s
// RUN: %clang --target=aarch64-windows-itanium -fdwarf-exceptions -c %s -### 2>&1 | FileCheck -check-prefixes=ITANIUM,ITANIUM-DWARF %s

MSVC-NOT: -exception-model=dwarf
MSVC-NOT: -exception-model=seh
MSVC-NOSEH-NOT: -funwind-tables=2
MSVC-SEH: -funwind-tables=2
MINGW-DWARF: -exception-model=dwarf
MINGW-SEH: -funwind-tables=2
MINGW-SEH: -exception-model=seh
ITANIUM-SJLJ: warning: ignoring '-fsjlj-exceptions' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
ITANIUM-DWARF: warning: ignoring '-fdwarf-exceptions' option as it is not currently supported for target 'aarch64-unknown-windows-itanium'
ITANIUM-NOT: -exception-model={{sjlj|dwarf}}
ITANIUM: -exception-model=seh
ITANIUM-NOT: -exception-model={{sjlj|dwarf}}
