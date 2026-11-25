// The Windows Itanium toolchain: compilation.

// RUN: rm -rf %t && split-file %s %t

// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-X64 %s \
// RUN:       --implicit-check-not=-fno-use-cxa-atexit \
// RUN:       --implicit-check-not=-fvisibility-global-new-delete \
// RUN:       --implicit-check-not=-fno-dllexport-inlines
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-A64 %s \
// RUN:       --implicit-check-not=-ehcontguard \
// RUN:       --implicit-check-not=-fno-use-cxa-atexit \
// RUN:       --implicit-check-not=-fvisibility-global-new-delete \
// RUN:       --implicit-check-not=-fno-dllexport-inlines
// CC1-X64:     "-cc1" "-triple" "x86_64-unknown-windows-itanium"
// CC1-A64:     "-cc1" "-triple" "aarch64-unknown-windows-itanium"
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
// CC1-X64-DAG: "-ehcontguard"

// Debug info is CodeView, for lld-link to write a PDB from.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c -g %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEBUG %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c -g %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEBUG %s
// DEBUG: "-gcodeview"

// The AArch64 hardware floor is a feature set, which -march= only tunes above.
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=A64-FLOOR %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -march=armv8-a 2>&1 | FileCheck --check-prefix=A64-FLOOR %s
// A64-FLOOR:      "-target-feature" "+v8.2a" "-target-feature" "+neon"
// A64-FLOOR-SAME: "-target-feature" "+crc" "-target-feature" "+lse"
// A64-FLOOR-SAME: "-target-feature" "+lse2" "-target-feature" "+rcpc"
// A64-FLOOR-SAME: "-target-feature" "+rcpc-immo" "-target-feature" "+flagm"
// A64-FLOOR-SAME: "-target-feature" "+dotprod" "-target-feature" "+fullfp16"
// A64-FLOOR-SAME: "-target-feature" "+rdm" "-target-feature" "+aes"
// A64-FLOOR-SAME: "-target-feature" "+sha2" "-target-feature" "+ccpp"
// A64-FLOOR-SAME: "-target-feature" "+pauth"

// PlayStation's flavour of Windows Itanium and the 32-bit targets keep the
// cross-Windows toolchain.
// RUN: %clang -### --target=x86_64-scei-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CROSS %s --implicit-check-not=-D_DLL \
// RUN:       --implicit-check-not=-Wget-proc-address-type
// RUN: %clang -### --target=i686-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CROSS %s --implicit-check-not=-D_DLL \
// RUN:       --implicit-check-not=-Wget-proc-address-type
// CROSS: "-cc1" "-triple" "{{x86_64-scei|i686-unknown}}-windows-itanium"

// The toolchain's defaults give way to the user's options, and without the
// Microsoft extensions there is no compatibility version.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-ms-extensions -fno-declspec -fno-use-cxa-atexit 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-DEFAULTS %s \
// RUN:       --implicit-check-not=-fms-extensions \
// RUN:       --implicit-check-not=-fms-compatibility-version \
// RUN:       --implicit-check-not=-fdeclspec
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -fms-compatibility-version=19.40 2>&1 \
// RUN:   | FileCheck --check-prefix=MS-VERSION %s \
// RUN:       --implicit-check-not=-fms-compatibility-version=19.33
// NO-DEFAULTS: "-fno-use-cxa-atexit"
// MS-VERSION:  "-fms-compatibility-version=19.40"

// clang-cl's /MT does not make the C++ runtime's classes local to the image,
// since libc++ is linked dynamically with either CRT.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /MT \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-MT %s \
// RUN:       --implicit-check-not=-flto-visibility-public-std
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c /MTd \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-MT %s \
// RUN:       --implicit-check-not=-flto-visibility-public-std
// CL-MT: "-D_MT"

// No sanitizer runtime is built for the target, so a check must trap, and
// cross-DSO CFI, which calls the runtime, is rejected.
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize=undefined 2>&1 \
// RUN:   | FileCheck --check-prefix=UBSAN-NO-TRAP %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize=undefined -fsanitize-trap=undefined 2>&1 \
// RUN:   | FileCheck --check-prefix=UBSAN-TRAP %s --implicit-check-not=error:
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -fvisibility=hidden -fsanitize=cfi -fsanitize-cfi-cross-dso \
// RUN:     -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=CFI-CROSS-DSO %s
// UBSAN-NO-TRAP: error: unsupported option '-fno-sanitize-trap={{[a-z,-]+}}' for target 'x86_64-unknown-windows-itanium'
// UBSAN-TRAP:    "-fsanitize-trap=
// CFI-CROSS-DSO: error: unsupported option '-fsanitize-cfi-cross-dso' for target 'x86_64-unknown-windows-itanium'

// Separate sections and unwind tables are not optional, so the options that
// would turn them off are ignored, and -ffreestanding keeps the tables.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-function-sections -fno-data-sections \
// RUN:     -fno-asynchronous-unwind-tables -fno-unwind-tables 2>&1 \
// RUN:   | FileCheck --check-prefixes=MANDATORY,MANDATORY-GCC %s \
// RUN:       --implicit-check-not="argument unused"
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c /Gy- /Gw- \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=MANDATORY,MANDATORY-CL %s \
// RUN:       --implicit-check-not="argument unused"
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -ffreestanding 2>&1 | FileCheck --check-prefix=MANDATORY %s
// MANDATORY-GCC:      warning: ignoring '-fno-function-sections' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-GCC-NEXT: warning: ignoring '-fno-data-sections' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-GCC-NEXT: warning: ignoring '-fno-asynchronous-unwind-tables' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-GCC-NEXT: warning: ignoring '-fno-unwind-tables' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// MANDATORY-CL:       warning: ignoring '/Gy-' option as it is not currently supported for target 'aarch64-unknown-windows-itanium'
// MANDATORY-CL-NEXT:  warning: ignoring '/Gw-' option as it is not currently supported for target 'aarch64-unknown-windows-itanium'
// MANDATORY-DAG:      "-funwind-tables=2"
// MANDATORY-DAG:      "-ffunction-sections"
// MANDATORY-DAG:      "-fdata-sections"

// -fno-stack-protector turns the stack protector off, and so does clang-cl's
// /GS- where it is the last of the options that set it.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-stack-protector 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-SSP %s \
// RUN:       --implicit-check-not=-stack-protector
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c /GS /GS- \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-SSP %s \
// RUN:       --implicit-check-not=-stack-protector
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /GS- /GS \
// RUN:     -- %s 2>&1 | FileCheck --check-prefix=CL-SSP %s
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c /GS- \
// RUN:     /clang:-fstack-protector-all -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-SSP-ALL %s
// NO-SSP:     "-cc1"
// CL-SSP:     "-stack-protector" "2"
// CL-SSP-ALL: "-stack-protector" "3"

// A user's -ftrivial-auto-var-init= replaces the default, which satisfies the
// options that need an initialization.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -ftrivial-auto-var-init=uninitialized 2>&1 \
// RUN:   | FileCheck --check-prefix=AUTO-INIT-USER %s \
// RUN:       --implicit-check-not=-ftrivial-auto-var-init=zero
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c \
// RUN:     -ftrivial-auto-var-init-max-size=1024 -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=AUTO-INIT-MAX-SIZE %s \
// RUN:       --implicit-check-not=error:
// AUTO-INIT-USER:          "-ftrivial-auto-var-init=uninitialized"
// AUTO-INIT-MAX-SIZE:      "-ftrivial-auto-var-init=zero"
// AUTO-INIT-MAX-SIZE-SAME: "-ftrivial-auto-var-init-max-size=1024"

// Control Flow Guard is on by default. -mguard=cf-nochecks and -mguard=none
// leave out its checks and keep its tables, and clang-cl's /guard: values
// select the same modes.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /guard:cf- \
// RUN:     /guard:cf -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CF %s \
// RUN:       --implicit-check-not=-cfguard-no-checks
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -mguard=cf-nochecks 2>&1 | FileCheck --check-prefix=CF-NOCHECKS %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -mguard=none 2>&1 | FileCheck --check-prefix=CF-NOCHECKS %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c \
// RUN:     /guard:cf,nochecks -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CF-NOCHECKS %s
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c /guard:cf \
// RUN:     /d2guardnochecks -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CF-NOCHECKS %s
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c /guard:cf- \
// RUN:     -- %s 2>&1 | FileCheck --check-prefix=CF-NOCHECKS %s
// CF:              "-cfguard"
// CF-NOCHECKS-NOT: "-cfguard"
// CF-NOCHECKS:     "-cfguard-no-checks"
// CF-NOCHECKS-NOT: "-cfguard"

// EH continuation metadata is not a choice: clang-cl's /guard:ehcont and
// /guard:ehcont- are accepted and change nothing, and -mguard= has no value
// for it.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c \
// RUN:     /guard:ehcont- -- %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CF,CL-EHCONT %s \
// RUN:       --implicit-check-not=warning:
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /c \
// RUN:     /guard:ehcont -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CF %s --implicit-check-not=warning: \
// RUN:       --implicit-check-not=-ehcontguard
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -mguard=ehcont 2>&1 | FileCheck --check-prefix=CF-UNKNOWN %s
// RUN: not %clang_cl -### --target=aarch64-unknown-windows-itanium /c \
// RUN:     /guard:foo -- %s 2>&1 | FileCheck --check-prefix=CL-CF-UNKNOWN %s
// CL-EHCONT:     "-ehcontguard"
// CF-UNKNOWN:    error: unsupported argument 'ehcont' to option '-mguard='
// CL-CF-UNKNOWN: error: invalid value 'foo' in '/guard:'

// The resource headers, the wrappers over the Universal CRT's and the Windows
// SDK's headers, then those headers, found as the MSVC toolchain finds them.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=INCLUDE %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=INCLUDE %s
// INCLUDE:      "-internal-isystem" "[[RES:[^"]*]]{{/|\\\\}}include"
// INCLUDE-SAME: "-internal-isystem" "[[RES]]{{/|\\\\}}include{{/|\\\\}}win32_itanium_wrappers"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT:[^"]*]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}shared"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}um"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}winrt"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}cppwinrt"

// -nostdlibinc leaves only the resource headers.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -Xmicrosoft-windows-sys-root %t -nostdlibinc 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIBINC %s \
// RUN:       --implicit-check-not=win32_itanium_wrappers \
// RUN:       --implicit-check-not="Windows Kits"
// NOSTDLIBINC: "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"

// clang-cl's /imsvc and /external:env: directories come ahead of the
// Universal CRT's and the Windows SDK's headers. %INCLUDE%, which names
// Visual C++'s, is never read.
// RUN: env "FOO=/dir1;/dir2" env INCLUDE=/vc/include \
// RUN:   %clang_cl -### --target=x86_64-unknown-windows-itanium /c \
// RUN:     /winsysroot %t /imsvc /dir0 /external:env:FOO -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-INCLUDE %s \
// RUN:       --implicit-check-not=/vc/include
// CL-INCLUDE:      "-internal-isystem" "/dir0"
// CL-INCLUDE-SAME: "-internal-isystem" "/dir1"
// CL-INCLUDE-SAME: "-internal-isystem" "/dir2"
// CL-INCLUDE-SAME: "-internal-isystem" "{{[^"]*}}{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt"

// The CUDA and HIP installations are found as the MSVC toolchain finds them.
// RUN: %clang -v --target=x86_64-unknown-windows-itanium \
// RUN:     --cuda-path=%S/Inputs/CUDA/usr/local/cuda \
// RUN:     --rocm-path=%S/Inputs/rocm 2>&1 \
// RUN:   | FileCheck --check-prefix=OFFLOAD-DETECT %s
// RUN: %clang -v --target=aarch64-unknown-windows-itanium \
// RUN:     --cuda-path=%S/Inputs/CUDA/usr/local/cuda \
// RUN:     --rocm-path=%S/Inputs/rocm 2>&1 \
// RUN:   | FileCheck --check-prefix=OFFLOAD-DETECT %s
// OFFLOAD-DETECT: Found CUDA installation: {{.*}}Inputs{{/|\\\\}}CUDA{{/|\\\\}}usr{{/|\\\\}}local{{/|\\\\}}cuda
// OFFLOAD-DETECT: Found HIP installation: {{.*}}Inputs{{/|\\\\}}rocm

// libc++'s headers, from the sysroot.
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     --sysroot=%t 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBCXX %s
// RUN: %clangxx -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     --sysroot=%t -nostdinc++ 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-LIBCXX %s --implicit-check-not=v1
// LIBCXX:    "-internal-isystem" "{{[^"]*}}.tmp{{/|\\\\}}include{{/|\\\\}}c++{{/|\\\\}}v1"
// NO-LIBCXX: "-cc1"

//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
//--- include/c++/v1/__config
#define _LIBCPP_VERSION 230000
