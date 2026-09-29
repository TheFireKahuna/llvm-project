// The NT-POSIX toolchain: compilation.

// RUN: rm -rf %t && split-file %s %t

// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-X64 %s \
// RUN:       --implicit-check-not=-fms-extensions \
// RUN:       --implicit-check-not=-fwchar-type \
// RUN:       --implicit-check-not=-D_DLL \
// RUN:       --implicit-check-not=-fno-use-cxa-atexit \
// RUN:       --implicit-check-not=-ehcontguard
// RUN: %clang -### --target=aarch64-pc-windows-ntposix -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s \
// RUN:       --implicit-check-not=-disable-red-zone
// CC1:         "-cc1" "-triple" "{{x86_64|aarch64}}-pc-windows-ntposix"
// CC1-DAG:     "-mdefault-visibility-export-mapping=explicit"
// CC1-DAG:     "-D_LIBC_DLL"
// CC1-DAG:     "-pthread"
// CC1-X64-DAG: "-disable-red-zone"
// CC1-DAG:     "-fdeclspec"
// CC1-DAG:     "-exception-model=seh"
// CC1-DAG:     "-cfguard"
// CC1-DAG:     "-ffunction-sections"
// CC1-DAG:     "-fdata-sections"
// CC1-DAG:     "-funwind-tables=2"

// A statically linked libc is not imported.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s -static 2>&1 \
// RUN:   | FileCheck --check-prefix=STATIC %s \
// RUN:       --implicit-check-not=-D_LIBC_DLL
// STATIC: "-cc1"

// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s -mred-zone 2>&1 \
// RUN:   | FileCheck --check-prefix=RED-ZONE %s \
// RUN:       --implicit-check-not=-disable-red-zone
// RED-ZONE: "-cc1"

// RUN: not %clang -### --target=x86_64-pc-windows-ntposix -c %s \
// RUN:     -fshort-wchar 2>&1 \
// RUN:   | FileCheck --check-prefix=SHORT-WCHAR %s
// SHORT-WCHAR: error: unsupported option '-fshort-wchar' for target 'x86_64-pc-windows-ntposix'

// Build systems written for POSIX systems pass -fPIC.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s -fPIC 2>&1 \
// RUN:   | FileCheck --check-prefix=PIC %s
// PIC-NOT: error:
// PIC:     "-cc1"

// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s \
// RUN:     -fsjlj-exceptions 2>&1 \
// RUN:   | FileCheck --check-prefix=SJLJ %s \
// RUN:       --implicit-check-not=-exception-model=sjlj
// SJLJ: warning: ignoring '-fsjlj-exceptions' option as it is not currently supported for target 'x86_64-pc-windows-ntposix'
// SJLJ: "-exception-model=seh"

// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s -mguard=cf 2>&1 \
// RUN:   | FileCheck --check-prefix=CF %s
// CF: "-cfguard"

// The resource headers, then llvm-libc's.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s --sysroot=%t 2>&1 \
// RUN:   | FileCheck --check-prefix=INCLUDE %s
// INCLUDE:      "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT:[^"]*]]{{/|\\\\}}include{{/|\\\\}}x86_64-pc-windows-ntposix"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}include"
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s --sysroot=%t \
// RUN:     -nostdlibinc 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIBINC %s
// NOSTDLIBINC:     "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"
// NOSTDLIBINC-NOT: "-internal-isystem"

//--- include/x86_64-pc-windows-ntposix/stdio.h
int puts(const char *);
//--- include/stdlib.h
void abort(void);
