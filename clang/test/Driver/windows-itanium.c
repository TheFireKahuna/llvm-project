// The Windows Itanium toolchain: compilation.

// RUN: rm -rf %t && split-file %s %t

// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-X64 %s \
// RUN:       --implicit-check-not=-fno-use-cxa-atexit \
// RUN:       --implicit-check-not=-fvisibility-global-new-delete \
// RUN:       --implicit-check-not=-fno-dllexport-inlines
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-A64 %s \
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
