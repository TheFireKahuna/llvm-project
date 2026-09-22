// REQUIRES: x86-registered-target

// Library lookup follows the linker's directory order and distinguishes an
// import library from the static archive with the same stem.
// RUN: rm -rf %t && mkdir -p %t/first %t/second
// RUN: touch %t/first/libboth.dll.lib %t/first/libboth.lib
// RUN: touch %t/first/libstatic.lib %t/first/native.lib
// RUN: touch %t/first/libordered.lib %t/second/libordered.dll.lib
// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdlib \
// RUN:   -L%t/first -L%t/second -lboth -lstatic -lnative -lordered \
// RUN:   -l:exact.lib -llibexplicit.lib -### %s 2>&1 | FileCheck %s --check-prefix=LIBS
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib \
// RUN:   -L%t/first -L%t/second -lboth -lstatic -lnative -lordered \
// RUN:   -l:exact.lib -llibexplicit.lib -### %s 2>&1 | FileCheck %s --check-prefix=LIBS
// LIBS: lld-link
// LIBS-SAME: "{{.*}}first{{[/\\]+}}libboth.dll.lib"
// LIBS-SAME: "{{.*}}first{{[/\\]+}}libstatic.lib"
// LIBS-SAME: "{{.*}}first{{[/\\]+}}native.lib"
// LIBS-SAME: "{{.*}}first{{[/\\]+}}libordered.lib"
// LIBS-SAME: "exact.lib"
// LIBS-SAME: "libexplicit.lib"

// Linker search paths apply even when they follow the -l option.
// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdlib -lboth \
// RUN:   -Wl,/libpath:%t/first -### %s 2>&1 | FileCheck %s --check-prefix=LATE-PATH
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -lboth \
// RUN:   -Wl,/libpath:%t/first -### %s 2>&1 | FileCheck %s --check-prefix=LATE-PATH
// LATE-PATH: lld-link
// LATE-PATH-SAME: "{{.*}}first{{[/\\]+}}libboth.dll.lib"

// The driver supplies the new default import-library suffix; an explicit
// linker option still overrides it.
// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdlib -shared \
// RUN:   -o libsample.dll -Wl,-implib:custom.lib -### %s 2>&1 | FileCheck %s --check-prefix=DLL
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -shared \
// RUN:   -o libsample.dll -Wl,-implib:custom.lib -### %s 2>&1 | FileCheck %s --check-prefix=DLL
// DLL: lld-link
// DLL-SAME: "-implib:libsample.dll.lib"
// DLL-SAME: "-implib:custom.lib"

// The standard module manifest is found beside the renamed import library.
// RUN: mkdir -p %t/modules
// RUN: touch %t/modules/libc++.dll.lib %t/modules/libc++.modules.json
// RUN: %clang --target=x86_64-unknown-windows-itanium -B%t/modules \
// RUN:   -print-library-module-manifest-path | FileCheck %s --check-prefix=MODULES
// RUN: %clang --target=x86_64-pc-windows-ntposix -B%t/modules \
// RUN:   -print-library-module-manifest-path | FileCheck %s --check-prefix=MODULES
// MODULES: {{.*}}modules{{[/\\]+}}libc++.modules.json
