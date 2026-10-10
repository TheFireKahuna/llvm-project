// The Windows Itanium toolchain: compilation.

// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-X64 %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-A64 %s
// CC1-X64:    "-cc1" "-triple" "x86_64-unknown-windows-itanium"
// CC1-A64:    "-cc1" "-triple" "aarch64-unknown-windows-itanium"
// CC1-DAG:    "-mdefault-visibility-export-mapping=explicit"
// CC1-DAG:    "-Wget-proc-address-type"
// CC1-DAG:    "-D_DLL"
// CC1-DAG:    "-fms-extensions"
// CC1-DAG:    "-fms-compatibility-version=19.33"
// CC1-DAG:    "-fdeclspec"
// CC1-DAG:    "-exception-model=seh"

// The SCEI flavour of Windows Itanium keeps the cross-Windows toolchain.
// RUN: %clang -### --target=x86_64-scei-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SCEI %s \
// RUN:       --implicit-check-not=-mdefault-visibility-export-mapping \
// RUN:       --implicit-check-not=-fsanitize=kcfi \
// RUN:       --implicit-check-not=-ehcontguard
// SCEI: "-cc1" "-triple" "x86_64-scei-windows-itanium"

// Flags the toolchain never passes by default. The Universal CRT's
// configuration is left to the wrapper headers and the project.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=FORBIDDEN %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=FORBIDDEN %s
// FORBIDDEN-NOT: "-D__MSVCRT__"
// FORBIDDEN-NOT: "-UCLOCK_REALTIME"
// FORBIDDEN-NOT: "-fno-dllexport-inlines"
// FORBIDDEN-NOT: "-fvisibility-global-new-delete"
// FORBIDDEN-NOT: "-fno-use-cxa-atexit"
// FORBIDDEN-NOT: "-fms-compatibility"
// FORBIDDEN-NOT: "-D_CRT_SECURE_NO_WARNINGS"
// FORBIDDEN-NOT: "-D_CRT_STDIO_ISO_WIDE_SPECIFIERS"

// A user's export mapping replaces the default one.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -mdefault-visibility-export-mapping=none 2>&1 \
// RUN:   | FileCheck --check-prefix=MAPPING %s \
// RUN:       --implicit-check-not=-mdefault-visibility-export-mapping=explicit
// MAPPING: "-mdefault-visibility-export-mapping=none"

// Without the Microsoft extensions there is no compatibility version.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-ms-extensions 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-MS %s \
// RUN:       --implicit-check-not=-fms-extensions \
// RUN:       --implicit-check-not=-fms-compatibility-version
// NO-MS: "-cc1"

// A user may still ask for the inline members of an exported class to stay
// unexported.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c \
// RUN:     /Zc:dllexportInlines- -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-INLINES %s
// NO-INLINES: "-fno-dllexport-inlines"

// clang-cl's /MT does not make the C++ runtime's functions local, since
// libc++ is linked dynamically with either CRT.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /MT \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-MT %s
// CL-MT:      "-cc1"
// CL-MT-SAME: "-D_MT"
// CL-MT-NOT:  "-flto-visibility-public-std"

// clang-cl's /std: selects the C++ standard; without it the target keeps
// clang's default rather than Visual C++'s.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /TP \
// RUN:     /std:c++20 -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-STD %s --implicit-check-not=warning:
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /TP \
// RUN:     /std:c++latest -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-STD-LATEST %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /TP \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-STD-DEFAULT %s
// CL-STD:              "-cc1"
// CL-STD-SAME:         "-std=c++20"
// CL-STD-LATEST:       "-cc1"
// CL-STD-LATEST-SAME:  "-std=c++2d"
// CL-STD-DEFAULT:      "-cc1"
// CL-STD-DEFAULT-NOT:  "-std=

// SEH is the only exception model.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fdwarf-exceptions 2>&1 \
// RUN:   | FileCheck --check-prefix=DWARF %s \
// RUN:       --implicit-check-not=-exception-model=dwarf
// DWARF: warning: ignoring '-fdwarf-exceptions' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// DWARF: "-exception-model=seh"

// The resource headers, the wrappers over the Universal CRT and Windows SDK
// headers, then those headers, found as the MSVC toolchain finds them.
// RUN: rm -rf %t && split-file %s %t
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=INCLUDE %s
// INCLUDE:      "-internal-isystem" "[[RES:[^"]*]]{{/|\\\\}}include"
// INCLUDE-SAME: "-internal-isystem" "[[RES]]{{/|\\\\}}include{{/|\\\\}}win32_itanium_wrappers"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT:[^"]*]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}shared"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}um"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}winrt"
// INCLUDE-SAME: "-internal-isystem" "[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}cppwinrt"

// Visual C++ is never looked for, and -nostdlibinc leaves only the resource
// headers.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -Xmicrosoft-windows-sys-root %t -nostdlibinc 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIBINC %s \
// RUN:       --implicit-check-not=win32_itanium_wrappers \
// RUN:       --implicit-check-not="Windows Kits"
// NOSTDLIBINC: "-internal-isystem" "{{[^"]*}}{{/|\\\\}}include"

// clang-cl's /external:env: adds the directories an environment variable
// lists, ahead of the Universal CRT and Windows SDK headers. %INCLUDE% is
// never read.
// RUN: env "FOO=/dir1;/dir2" env "BAR=/dir3" env INCLUDE=/vc/include \
// RUN:   %clang_cl -### --target=x86_64-unknown-windows-itanium /c \
// RUN:     /winsysroot %t /external:env:FOO /external:env:BAR -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=EXTERNAL-ENV %s \
// RUN:       --implicit-check-not=/vc/include
// EXTERNAL-ENV:      "-internal-isystem" "/dir1"
// EXTERNAL-ENV-SAME: "-internal-isystem" "/dir2"
// EXTERNAL-ENV-SAME: "-internal-isystem" "/dir3"
// EXTERNAL-ENV-SAME: "-internal-isystem" "{{[^"]*}}{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt"

// The CUDA and HIP installations are found as the MSVC toolchain finds them.
// RUN: %clang -v --target=x86_64-unknown-windows-itanium \
// RUN:     --cuda-path=%S/Inputs/CUDA/usr/local/cuda \
// RUN:     --rocm-path=%S/Inputs/rocm 2>&1 \
// RUN:   | FileCheck --check-prefix=OFFLOAD-DETECT %s
// OFFLOAD-DETECT: Found CUDA installation: {{.*}}Inputs{{/|\\\\}}CUDA{{/|\\\\}}usr{{/|\\\\}}local{{/|\\\\}}cuda
// OFFLOAD-DETECT: Found HIP installation: {{.*}}Inputs{{/|\\\\}}rocm

// libc++'s headers, from the sysroot.
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     --sysroot=%t 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBCXX %s
// LIBCXX: "-internal-isystem" "{{[^"]*}}.tmp{{/|\\\\}}include{{/|\\\\}}c++{{/|\\\\}}v1"

// Linking uses lld-link, never the library path of a Visual Studio developer
// shell.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-X64 %s
// LINK-X64: lld-link{{(.exe)?}}"
// LINK-X64-SAME: "-machine:x64"
// LINK-X64-SAME: "-lldignoreenv"
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -shared %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-A64 %s
// LINK-A64: lld-link{{(.exe)?}}"
// LINK-A64-SAME: "-machine:arm64"
// LINK-A64-SAME: "-dll"


//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
//--- include/c++/v1/__config
#define _LIBCPP_VERSION 230000
