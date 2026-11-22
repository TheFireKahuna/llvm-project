// The Windows Itanium toolchain: compilation.

// RUN: rm -rf %t && split-file %s %t
// DEFINE: %{x64} = %clang -### --target=x86_64-unknown-windows-itanium -c %s
// DEFINE: %{a64} = %clang -### --target=aarch64-unknown-windows-itanium -c %s
// DEFINE: %{cl-x64} = %clang_cl -### --target=x86_64-unknown-windows-itanium /c
// DEFINE: %{cl-a64} = %clang_cl -### --target=aarch64-unknown-windows-itanium /c
// DEFINE: %{itanium} = --implicit-check-not=-fno-use-cxa-atexit \
// DEFINE:   --implicit-check-not=-fvisibility-global-new-delete --implicit-check-not=-fno-dllexport-inlines \
// DEFINE:   --implicit-check-not=semantic-interposition

// The defaults. The AArch64 hardware floor is a feature set, which -march=
// only tunes above.
// RUN: %{x64} 2>&1 | FileCheck --check-prefixes=CC1,CC1-X64 %s %{itanium}
// RUN: %{a64} -march=armv8-a 2>&1 | FileCheck --check-prefixes=CC1,CC1-A64 %s %{itanium} \
// RUN:   --implicit-check-not=-ehcontguard
// CC1-X64:     "-cc1" "-triple" "x86_64-unknown-windows-itanium"
// CC1-A64:     "-cc1" "-triple" "aarch64-unknown-windows-itanium"
// CC1-A64-DAG: "-target-feature" "+v8.2a" "-target-feature" "+neon"
// CC1-A64-DAG: "-target-feature" "+crc" "-target-feature" "+lse"
// CC1-A64-DAG: "-target-feature" "+lse2" "-target-feature" "+rcpc"
// CC1-A64-DAG: "-target-feature" "+rcpc-immo" "-target-feature" "+flagm"
// CC1-A64-DAG: "-target-feature" "+dotprod" "-target-feature" "+fullfp16"
// CC1-A64-DAG: "-target-feature" "+rdm" "-target-feature" "+aes"
// CC1-A64-DAG: "-target-feature" "+sha2" "-target-feature" "+ccpp"
// CC1-A64-DAG: "-target-feature" "+pauth"
// CC1-DAG:     "-pic-level" "2"
// CC1-DAG:     "-Wget-proc-address-type"
// CC1-DAG:     "-D_DLL"
// CC1-DAG:     "-fms-extensions"
// CC1-DAG:     "-fms-compatibility-version=19.33"
// CC1-DAG:     "-fdeclspec"
// CC1-DAG:     "-exception-model=seh"
// CC1-DAG:     "-funwind-tables=2"
// CC1-DAG:     "-ffunction-sections"
// CC1-DAG:     "-fdata-sections"
// CC1-DAG:     "-stack-protector" "2"
// CC1-DAG:     "-ftrivial-auto-var-init=zero"
// CC1-DAG:     "-cfguard"
// CC1-DAG:     "-fgnu-unique"
// CC1-DAG:     "-fsanitize-cfi-icall-generalize-pointers"
// CC1-DAG:     "-mdefault-visibility-export-mapping=explicit"
// CC1-X64-DAG: "-ehcontguard"

// PlayStation's flavour of Windows Itanium and the 32-bit targets keep the
// cross-Windows toolchain.
// RUN: %clang -### --target=x86_64-scei-windows-itanium -c %s 2>&1 | FileCheck --check-prefix=CROSS %s \
// RUN:   --implicit-check-not=-D_DLL --implicit-check-not=-Wget-proc-address-type
// RUN: %clang -### --target=i686-unknown-windows-itanium -c %s 2>&1 | FileCheck --check-prefix=CROSS %s \
// RUN:   --implicit-check-not=-D_DLL --implicit-check-not=-Wget-proc-address-type
// CROSS: "-cc1" "-triple" "{{x86_64-scei|i686-unknown}}-windows-itanium"

// The toolchain's defaults give way to the user's options; without the
// Microsoft extensions there is no compatibility version. Debug information is
// CodeView, for lld-link to write a PDB from. No sanitizer runtime is built
// for the target, so a check must trap.
// RUN: %{x64} -fno-ms-extensions -fno-declspec -fno-use-cxa-atexit -fno-stack-protector \
// RUN:   -ftrivial-auto-var-init=uninitialized -mguard=none -g -fsanitize=undefined \
// RUN:   -fsanitize-trap=undefined -fno-gnu-unique -mdefault-visibility-export-mapping=all 2>&1 \
// RUN:   | FileCheck --check-prefix=USER %s --implicit-check-not=error: --implicit-check-not=mapping=explicit \
// RUN:   --implicit-check-not=-fgnu-unique \
// RUN:   --implicit-check-not=-fms-extensions --implicit-check-not=-fms-compatibility-version \
// RUN:   --implicit-check-not=-fdeclspec --implicit-check-not=-stack-protector \
// RUN:   --implicit-check-not=-ftrivial-auto-var-init=zero --implicit-check-not='"-cfguard"'
// USER-DAG: "-fno-use-cxa-atexit"
// USER-DAG: "-ftrivial-auto-var-init=uninitialized"
// USER-DAG: "-cfguard-no-checks"
// USER-DAG: "-gcodeview"
// USER-DAG: "-mdefault-visibility-export-mapping=all"
// USER-DAG: "-fsanitize-trap=

// A user's -fms-compatibility-version= and -ftrivial-auto-var-init-max-size=,
// which needs the default initialization, apply. -mguard=cf-nochecks leaves out
// Control Flow Guard's checks and keeps its tables. A function's type id is the
// target's: it always ignores pointee types, with or without KCFI's checks.
// RUN: %{a64} -fms-compatibility-version=19.40 -ftrivial-auto-var-init-max-size=1024 \
// RUN:   -mguard=cf-nochecks -fsanitize-cfi-icall-generalize-pointers -fsanitize=kcfi \
// RUN:   -fsanitize-kcfi-hash=xxHash64 2>&1 \
// RUN:   | FileCheck --check-prefix=ACCEPT %s --implicit-check-not=error: --implicit-check-not=warning: \
// RUN:   --implicit-check-not=-fms-compatibility-version=19.33 --implicit-check-not='"-cfguard"'
// ACCEPT-DAG: "-fms-compatibility-version=19.40"
// ACCEPT-DAG: "-ftrivial-auto-var-init=zero"
// ACCEPT-DAG: "-ftrivial-auto-var-init-max-size=1024"
// ACCEPT-DAG: "-cfguard-no-checks"
// ACCEPT-DAG: "-fsanitize-cfi-icall-generalize-pointers"
// ACCEPT-DAG: "-fsanitize-kcfi-hash=xxHash64"

// -fsemantic-interposition reaches the compiler, as on ELF, and the last of it
// and -fno-semantic-interposition wins.
// RUN: %{a64} -fno-semantic-interposition -fsemantic-interposition 2>&1 \
// RUN:   | FileCheck --check-prefix=INTERPOSE %s
// RUN: %{x64} -fsemantic-interposition -fno-semantic-interposition 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s --implicit-check-not=semantic-interposition \
// RUN:   --implicit-check-not="argument unused"
// INTERPOSE: "-fsemantic-interposition"

// Separate sections and unwind tables are not optional, so the options that
// would turn them off are ignored, and -ffreestanding keeps the tables.
// RUN: %{x64} -fno-function-sections -fno-data-sections -fno-asynchronous-unwind-tables \
// RUN:   -fno-unwind-tables -ffreestanding 2>&1 | FileCheck --check-prefix=MANDATORY %s \
// RUN:   --implicit-check-not="argument unused"
// MANDATORY:      warning: ignoring '-fno-function-sections' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-NEXT: warning: ignoring '-fno-data-sections' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-NEXT: warning: ignoring '-fno-asynchronous-unwind-tables' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-NEXT: warning: ignoring '-fno-unwind-tables' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-DAG:  "-funwind-tables=2"
// MANDATORY-DAG:  "-ffunction-sections"
// MANDATORY-DAG:  "-fdata-sections"

// The options that would change a function's type id are errors, and so is a
// hot patch, which would overwrite it; -mguard= has no value for EH
// continuation metadata, which is not a choice; cross-DSO CFI calls a runtime
// the target does not have.
// RUN: not %{x64} -fsanitize-cfi-icall-experimental-normalize-integers -fsanitize-kcfi-arity \
// RUN:   -fsanitize-kcfi-hash=FNV-1a -fms-hotpatch -mguard=ehcont -fsanitize=undefined 2>&1 \
// RUN:   | FileCheck --check-prefix=ERRORS %s
// RUN: not %{x64} -flto -fvisibility=hidden -fsanitize=cfi -fsanitize-cfi-cross-dso \
// RUN:   -resource-dir=%S/Inputs/resource_dir 2>&1 | FileCheck --check-prefix=CROSS-DSO %s
// ERRORS-DAG: error: unsupported option '-fsanitize-cfi-icall-experimental-normalize-integers' for target 'x86_64-unknown-windows-itanium'
// ERRORS-DAG: error: unsupported option '-fsanitize-kcfi-arity' for target 'x86_64-unknown-windows-itanium'
// ERRORS-DAG: error: unsupported option '-fsanitize-kcfi-hash=FNV-1a' for target 'x86_64-unknown-windows-itanium'
// ERRORS-DAG: error: unsupported option '-fms-hotpatch' for target 'x86_64-unknown-windows-itanium'
// ERRORS-DAG: error: unsupported argument 'ehcont' to option '-mguard='
// ERRORS-DAG: error: unsupported option '-fno-sanitize-trap={{[a-z,-]+}}' for target 'x86_64-unknown-windows-itanium'
// CROSS-DSO:  error: unsupported option '-fsanitize-cfi-cross-dso' for target 'x86_64-unknown-windows-itanium'

// Where KCFI checks calls, a hot patch is the sanitizer's one error; clang-cl's
// /hotpatch is the target's.
// RUN: not %{a64} -fsanitize=kcfi -fms-hotpatch 2>&1 | FileCheck --check-prefix=KCFI-HOTPATCH %s \
// RUN:   --implicit-check-not="unsupported option"
// RUN: not %{cl-a64} /hotpatch -- %s 2>&1 | FileCheck --check-prefix=CL-HOTPATCH %s
// KCFI-HOTPATCH: error: invalid argument '-fsanitize=kcfi' not allowed with '-fms-hotpatch'
// CL-HOTPATCH:   error: unsupported option '/hotpatch' for target 'aarch64-unknown-windows-itanium'

// clang-cl: /MT does not make the C++ runtime's classes local to the image,
// since libc++ is linked dynamically with either CRT; /GS- and /guard:cf- turn
// the stack protector and the guard's checks off; /guard:ehcont- changes
// nothing; /Gy- and /Gw- are ignored. /imsvc and /external:env: directories
// come ahead of the Universal CRT's and the Windows SDK's headers, and
// %INCLUDE%, which names Visual C++'s, is never read.
// RUN: env "FOO=/dir1;/dir2" env INCLUDE=/vc/include %{cl-x64} /MT /GS /GS- /guard:cf- \
// RUN:   /guard:ehcont- /Gy- /Gw- /winsysroot %t /imsvc /dir0 /external:env:FOO -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL %s --implicit-check-not=/vc/include \
// RUN:   --implicit-check-not=-flto-visibility-public-std --implicit-check-not=-stack-protector \
// RUN:   --implicit-check-not='"-cfguard"' --implicit-check-not="argument unused"
// CL:      warning: ignoring '/Gy-' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// CL-NEXT: warning: ignoring '/Gw-' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// CL-DAG:  "-D_MT"
// CL-DAG:  "-cfguard-no-checks"
// CL-DAG:  "-ehcontguard"
// CL:      "-internal-isystem" "/dir0"
// CL-SAME: "-internal-isystem" "/dir1"
// CL-SAME: "-internal-isystem" "/dir2"
// CL-SAME: "-internal-isystem" "{{[^"]*}}{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt"

// The last of clang-cl's options wins, and /guard:ehcont is accepted.
// RUN: %{cl-a64} /GS- /GS /guard:cf- /guard:cf /guard:ehcont -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-LAST %s --implicit-check-not=warning: \
// RUN:   --implicit-check-not=-cfguard-no-checks --implicit-check-not=-ehcontguard
// RUN: %{cl-x64} /guard:cf,nochecks -- %s 2>&1 | FileCheck --check-prefix=CL-NOCHECKS %s
// RUN: %{cl-a64} /guard:cf /d2guardnochecks /GS- /clang:-fstack-protector-all -- %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CL-NOCHECKS,CL-SSP-ALL %s
// CL-LAST-DAG:     "-stack-protector" "2"
// CL-LAST-DAG:     "-cfguard"
// CL-NOCHECKS-NOT: "-cfguard"
// CL-NOCHECKS:     "-cfguard-no-checks"
// CL-NOCHECKS-NOT: "-cfguard"
// CL-SSP-ALL-DAG:  "-stack-protector" "3"

// The resource headers, the wrappers over the Universal CRT's and the Windows
// SDK's headers, then those headers, and the CUDA and HIP installations, found
// as the MSVC toolchain finds them; -nostdlibinc leaves only the resource
// headers.
// RUN: %{x64} -Xmicrosoft-windows-sys-root %t 2>&1 | FileCheck --check-prefix=INCLUDE %s
// RUN: %clang -v --target=x86_64-unknown-windows-itanium --cuda-path=%S/Inputs/CUDA/usr/local/cuda \
// RUN:   --rocm-path=%S/Inputs/rocm 2>&1 | FileCheck --check-prefix=OFFLOAD %s
// RUN: %{x64} -Xmicrosoft-windows-sys-root %t -nostdlibinc 2>&1 | FileCheck --check-prefix=NOSTDLIBINC %s \
// RUN:   --implicit-check-not=win32_itanium_wrappers --implicit-check-not="Windows Kits"
// INCLUDE:      "-internal-isystem" "[[RES:[^"]*]]{{/|\\\\}}include"
// INCLUDE-SAME: "-internal-isystem" "[[RES]]{{/|\\\\}}include{{/|\\\\}}win32_itanium_wrappers"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT:[^"]*]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}shared"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}um"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}winrt"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}cppwinrt"
// NOSTDLIBINC:  "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"
// OFFLOAD:      Found CUDA installation: {{.*}}Inputs{{/|\\\\}}CUDA{{/|\\\\}}usr{{/|\\\\}}local{{/|\\\\}}cuda
// OFFLOAD:      Found HIP installation: {{.*}}Inputs{{/|\\\\}}rocm

// libc++'s headers, from the sysroot.
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium -c %s --sysroot=%t 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBCXX %s
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium -c %s --sysroot=%t -nostdinc++ 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-LIBCXX %s --implicit-check-not=v1
// LIBCXX:    "-internal-isystem" "{{[^"]*}}.tmp{{/|\\\\}}include{{/|\\\\}}c++{{/|\\\\}}v1"
// NO-LIBCXX: "-cc1"

//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
//--- include/c++/v1/__config
#define _LIBCPP_VERSION 230000
