// The NT-POSIX toolchain: compilation.

// RUN: rm -rf %t && split-file %s %t

// DEFINE: %{x64} = %clang -### --target=x86_64-pc-windows-ntposix -c %s
// DEFINE: %{a64} = %clang -### --target=aarch64-pc-windows-ntposix -c %s
// DEFINE: %{posix} = --implicit-check-not=-stack-protector --implicit-check-not=-fms-extensions \
// DEFINE:   --implicit-check-not=-D_DLL --implicit-check-not=-fno-use-cxa-atexit \
// DEFINE:   --implicit-check-not=-fwchar-type --implicit-check-not=-fvisibility-global-new-delete \
// DEFINE:   --implicit-check-not=-fno-dllexport-inlines

// RUN: %{x64} 2>&1 | FileCheck --check-prefix=CC1 %s %{posix}
// RUN: %{a64} 2>&1 | FileCheck --check-prefixes=CC1,CC1-A64 %s %{posix}
// CC1:         "-cc1" "-triple" "{{x86_64|aarch64}}-pc-windows-ntposix"
// CC1-DAG:     "-pic-level" "2"
// CC1-DAG:     "-Wget-proc-address-type"
// CC1-DAG:     "-mframe-pointer=non-leaf"
// CC1-DAG:     "-funwind-tables=2"
// CC1-DAG:     "-ffunction-sections"
// CC1-DAG:     "-fdata-sections"
// CC1-DAG:     "-ftrivial-auto-var-init=zero"
// CC1-DAG:     "-pthread"
// CC1-DAG:     "-fdeclspec"
// CC1-DAG:     "-exception-model=seh"
// CC1-DAG:     "-cfguard-no-checks"
// CC1-DAG:     "-fsanitize-cfi-icall-generalize-pointers"
// CC1-DAG:     "-ehcontguard"
// CC1-A64-DAG: "-target-feature" "+lse2"

// A user's frame-pointer options still apply; Control Flow Guard's checks are
// not yet a default; build systems written for POSIX pass -fPIC.
// RUN: %{x64} -mno-omit-leaf-frame-pointer -mguard=cf -fPIC 2>&1 | FileCheck --check-prefix=USER %s \
// RUN:   --implicit-check-not=error:
// USER-DAG: "-mframe-pointer=all"
// USER-DAG: "-cfguard"
// USER-DAG: "-pic-level" "2"

// Separate sections and unwind tables are not optional.
// RUN: %{a64} -ffreestanding -fno-data-sections -fno-unwind-tables 2>&1 | FileCheck --check-prefix=MANDATORY %s
// MANDATORY:      warning: ignoring '-fno-data-sections' option as it is not currently supported for target 'aarch64-pc-windows-ntposix'
// MANDATORY-NEXT: warning: ignoring '-fno-unwind-tables' option as it is not currently supported for target 'aarch64-pc-windows-ntposix'
// MANDATORY-DAG:  "-funwind-tables=2"
// MANDATORY-DAG:  "-fdata-sections"

// The options that would change a function's type id are errors, and so is a
// hot patch, which would overwrite it. A 16-bit wchar_t is the Win32 face's,
// not an option's.
// RUN: not %{a64} -fsanitize-cfi-icall-experimental-normalize-integers -fsanitize-kcfi-arity \
// RUN:   -fsanitize-kcfi-hash=FNV-1a -fms-hotpatch -fshort-wchar 2>&1 | FileCheck --check-prefix=ERRORS %s
// ERRORS-DAG: error: unsupported option '-fsanitize-cfi-icall-experimental-normalize-integers' for target 'aarch64-pc-windows-ntposix'
// ERRORS-DAG: error: unsupported option '-fsanitize-kcfi-arity' for target 'aarch64-pc-windows-ntposix'
// ERRORS-DAG: error: unsupported option '-fsanitize-kcfi-hash=FNV-1a' for target 'aarch64-pc-windows-ntposix'
// ERRORS-DAG: error: unsupported option '-fms-hotpatch' for target 'aarch64-pc-windows-ntposix'
// ERRORS-DAG: error: unsupported option '-fshort-wchar' for target 'aarch64-pc-windows-ntposix'

// The resource headers, then the C library's, the target's own first.
// RUN: %{x64} --sysroot=%t 2>&1 | FileCheck --check-prefix=INCLUDE %s \
// RUN:   --implicit-check-not=win32_itanium_wrappers --implicit-check-not="Windows Kits"
// RUN: %{x64} --sysroot=%t -nostdlibinc 2>&1 | FileCheck --check-prefix=NOSTDLIBINC %s
// INCLUDE:      "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT:[^"]*]]{{/|\\\\}}include{{/|\\\\}}x86_64-pc-windows-ntposix"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}include"
// NOSTDLIBINC:     "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"
// NOSTDLIBINC-NOT: "-internal-isystem"

//--- include/x86_64-pc-windows-ntposix/stdio.h
int puts(const char *);
//--- include/stdlib.h
void abort(void);
