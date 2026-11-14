// The NT-POSIX toolchain: compilation.

// RUN: rm -rf %t && split-file %s %t

// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s \
// RUN:       --implicit-check-not=-fms-extensions --implicit-check-not=-D_DLL \
// RUN:       --implicit-check-not=-fno-use-cxa-atexit \
// RUN:       --implicit-check-not=-fwchar-type \
// RUN:       --implicit-check-not=-fvisibility-global-new-delete \
// RUN:       --implicit-check-not=-fno-dllexport-inlines
// RUN: %clang -### --target=aarch64-pc-windows-ntposix -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-A64 %s \
// RUN:       --implicit-check-not=-fms-extensions --implicit-check-not=-D_DLL \
// RUN:       --implicit-check-not=-fno-use-cxa-atexit \
// RUN:       --implicit-check-not=-fwchar-type \
// RUN:       --implicit-check-not=-fvisibility-global-new-delete \
// RUN:       --implicit-check-not=-fno-dllexport-inlines
// CC1:         "-cc1" "-triple" "{{x86_64|aarch64}}-pc-windows-ntposix"
// CC1-DAG:     "-pic-level" "2"
// CC1-DAG:     "-Wget-proc-address-type"
// CC1-DAG:     "-mframe-pointer=non-leaf"
// CC1-DAG:     "-funwind-tables=2"
// CC1-DAG:     "-pthread"
// CC1-DAG:     "-fdeclspec"
// CC1-DAG:     "-exception-model=seh"
// CC1-A64-DAG: "-target-feature" "+lse2"

// A user's frame-pointer options still apply.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s \
// RUN:     -mno-omit-leaf-frame-pointer 2>&1 \
// RUN:   | FileCheck --check-prefix=LEAF-FP %s
// LEAF-FP: "-mframe-pointer=all"

// A 16-bit wchar_t is the Win32 face's, not an option's.
// RUN: not %clang -### --target=x86_64-pc-windows-ntposix -c %s \
// RUN:     -fshort-wchar 2>&1 \
// RUN:   | FileCheck --check-prefix=SHORT-WCHAR -DARCH=x86_64 %s
// RUN: not %clang -### --target=aarch64-pc-windows-ntposix -c %s \
// RUN:     -fshort-wchar 2>&1 \
// RUN:   | FileCheck --check-prefix=SHORT-WCHAR -DARCH=aarch64 %s
// SHORT-WCHAR: error: unsupported option '-fshort-wchar' for target '[[ARCH]]-pc-windows-ntposix'

// Build systems written for POSIX pass -fPIC.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s -fPIC 2>&1 \
// RUN:   | FileCheck --check-prefix=PIC %s --implicit-check-not=error:
// PIC: "-pic-level" "2"

// The resource headers, then the C library's, the target's own first.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s --sysroot=%t 2>&1 \
// RUN:   | FileCheck --check-prefix=INCLUDE %s \
// RUN:       --implicit-check-not=win32_itanium_wrappers \
// RUN:       --implicit-check-not="Windows Kits"
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %s --sysroot=%t \
// RUN:     -nostdlibinc 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIBINC %s
// INCLUDE:      "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT:[^"]*]]{{/|\\\\}}include{{/|\\\\}}x86_64-pc-windows-ntposix"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}include"
// NOSTDLIBINC:     "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"
// NOSTDLIBINC-NOT: "-internal-isystem"

//--- include/x86_64-pc-windows-ntposix/stdio.h
int puts(const char *);
//--- include/stdlib.h
void abort(void);
