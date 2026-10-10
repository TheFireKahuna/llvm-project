// The Windows Itanium toolchain: compilation.

// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-X64 %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CC1,CC1-A64 %s \
// RUN:       --implicit-check-not=-ehcontguard --implicit-check-not=-fno-plt
// CC1-X64:    "-cc1" "-triple" "x86_64-unknown-windows-itanium"
// CC1-A64:    "-cc1" "-triple" "aarch64-unknown-windows-itanium"
// CC1-DAG:    "-mdefault-visibility-export-mapping=explicit"
// CC1-DAG:    "-fno-auto-import"
// CC1-DAG:    "-Wget-proc-address-type"
// CC1-X64-DAG: "-fno-plt"
// CC1-DAG:    "-D_DLL"
// CC1-DAG:    "-fms-extensions"
// CC1-DAG:    "-fms-compatibility-version=19.33"
// CC1-DAG:    "-fdeclspec"
// CC1-DAG:    "-exception-model=seh"
// CC1-X64-DAG: "-ehcontguard"
// CC1-DAG:    "-ffunction-sections"
// CC1-DAG:    "-fdata-sections"
// CC1-DAG:    "-funwind-tables=2"
// CC1-DAG:    "-stack-protector" "2"
// CC1-DAG:    "-ftrivial-auto-var-init=zero"
// CC1-DAG:    "-fsanitize=kcfi"
// CC1-DAG:    "-fsanitize-cfi-icall-generalize-pointers"

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

// -fplt, -fno-plt and -fauto-import replace the defaults.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s -fplt \
// RUN:     -fauto-import 2>&1 | FileCheck --check-prefix=PLT %s \
// RUN:       --implicit-check-not=-fno-plt --implicit-check-not=-fno-auto-import
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s -fno-plt \
// RUN:     2>&1 | FileCheck --check-prefix=NO-PLT %s
// PLT:    "-cc1"
// NO-PLT: "-fno-plt"

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

// Every function and every data item has a section of its own.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-function-sections -fno-data-sections 2>&1 \
// RUN:   | FileCheck --check-prefixes=SECTIONS,SECTIONS-ALL %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /Gy- /Gw- \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=SECTIONS-CL,SECTIONS-ALL %s
// SECTIONS-NOT:    warning: argument unused
// SECTIONS:        warning: ignoring '-fno-function-sections' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// SECTIONS-NEXT:   warning: ignoring '-fno-data-sections' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// SECTIONS-NOT:    warning: argument unused
// SECTIONS-CL:     warning: ignoring '/Gy-' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// SECTIONS-CL-NEXT: warning: ignoring '/Gw-' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// SECTIONS-ALL:      "-cc1"
// SECTIONS-ALL-SAME: "-ffunction-sections"
// SECTIONS-ALL-SAME: "-fdata-sections"

// Unwind tables are always emitted.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-asynchronous-unwind-tables -fno-unwind-tables 2>&1 \
// RUN:   | FileCheck --check-prefix=UNWIND %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -ffreestanding 2>&1 \
// RUN:   | FileCheck --check-prefix=UNWIND-FREESTANDING %s
// UNWIND-NOT:  warning: argument unused
// UNWIND:      warning: ignoring '-fno-asynchronous-unwind-tables' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// UNWIND-NEXT: warning: ignoring '-fno-unwind-tables' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// UNWIND-NOT:  warning: argument unused
// UNWIND:      "-funwind-tables=2"
// UNWIND-FREESTANDING: "-funwind-tables=2"

// -fstack-protector-strong by default, which -fno-stack-protector and
// clang-cl's /GS- turn off.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-stack-protector 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-SSP %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /GS- \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-SSP %s
// NO-SSP:     "-cc1"
// NO-SSP-NOT: "-stack-protector"
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c /GS- /GS \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-SSP %s
// CL-SSP: "-stack-protector" "2"

// Automatic variables start zeroed by default, in both drivers; a user's
// -ftrivial-auto-var-init= wins, uninitialized included.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=AUTO-INIT-ZERO %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -ftrivial-auto-var-init=uninitialized 2>&1 \
// RUN:   | FileCheck --check-prefix=AUTO-INIT-UNINIT %s \
// RUN:       --implicit-check-not=-ftrivial-auto-var-init=zero
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /c \
// RUN:     -ftrivial-auto-var-init=uninitialized -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=AUTO-INIT-UNINIT %s \
// RUN:       --implicit-check-not=-ftrivial-auto-var-init=zero
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -ftrivial-auto-var-init=pattern 2>&1 \
// RUN:   | FileCheck --check-prefix=AUTO-INIT-PATTERN %s \
// RUN:       --implicit-check-not=-ftrivial-auto-var-init=zero
// AUTO-INIT-ZERO:    "-ftrivial-auto-var-init=zero"
// AUTO-INIT-UNINIT:  "-ftrivial-auto-var-init=uninitialized"
// AUTO-INIT-PATTERN: "-ftrivial-auto-var-init=pattern"

// The zero default satisfies the options that need an initialization kind.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -ftrivial-auto-var-init-stop-after=1 \
// RUN:     -ftrivial-auto-var-init-max-size=1024 2>&1 \
// RUN:   | FileCheck --check-prefix=AUTO-INIT-LIMITS %s \
// RUN:       --implicit-check-not=error:
// AUTO-INIT-LIMITS:     "-cc1"
// AUTO-INIT-LIMITS-DAG: "-ftrivial-auto-var-init=zero"
// AUTO-INIT-LIMITS-DAG: "-ftrivial-auto-var-init-stop-after=1"
// AUTO-INIT-LIMITS-DAG: "-ftrivial-auto-var-init-max-size=1024"

// -fno-sanitize=kcfi removes the checks; -cc1 keeps every function's prefix,
// typed as with the checks.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-sanitize=kcfi 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-KCFI %s --implicit-check-not=-fsanitize=kcfi
// NO-KCFI:     "-cc1"
// NO-KCFI-SAME: "-fsanitize-cfi-icall-generalize-pointers"

// A hot patch would overwrite the prefix, with or without the checks, and is
// reported once.
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-sanitize=kcfi -fms-hotpatch 2>&1 \
// RUN:   | FileCheck --check-prefix=HOTPATCH %s --implicit-check-not=error:
// RUN: not %clang -### --target=aarch64-unknown-windows-itanium -c %s \
// RUN:     -fno-sanitize=kcfi -fms-hotpatch 2>&1 \
// RUN:   | FileCheck --check-prefix=HOTPATCH-A64 %s --implicit-check-not=error:
// RUN: not %clang_cl -### --target=x86_64-unknown-windows-itanium /c /hotpatch \
// RUN:     -fno-sanitize=kcfi -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=HOTPATCH-CL %s --implicit-check-not=error:
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fms-hotpatch 2>&1 \
// RUN:   | FileCheck --check-prefix=HOTPATCH-KCFI %s --implicit-check-not=error:
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize=kcfi -fms-hotpatch 2>&1 \
// RUN:   | FileCheck --check-prefix=HOTPATCH-KCFI %s --implicit-check-not=error:
// HOTPATCH: error: unsupported option '-fms-hotpatch' for target 'x86_64-unknown-windows-itanium'
// HOTPATCH-A64: error: unsupported option '-fms-hotpatch' for target 'aarch64-unknown-windows-itanium'
// HOTPATCH-CL: error: unsupported option '/hotpatch' for target 'x86_64-unknown-windows-itanium'
// HOTPATCH-KCFI: error: invalid argument '-fsanitize=kcfi' not allowed with '-fms-hotpatch'

// The target fixes the definition of every function's type, with or without
// the checks, so an option that would change it is an error. Pointer
// generalisation and the default hash are the target's own.
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize-kcfi-hash=FNV-1a 2>&1 \
// RUN:   | FileCheck --check-prefix=KCFI-HASH %s
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize-cfi-icall-experimental-normalize-integers 2>&1 \
// RUN:   | FileCheck --check-prefix=KCFI-NORMALIZE %s
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fno-sanitize=kcfi -fsanitize-kcfi-arity 2>&1 \
// RUN:   | FileCheck --check-prefix=KCFI-ARITY %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize-kcfi-hash=xxHash64 \
// RUN:     -fsanitize-cfi-icall-generalize-pointers 2>&1 \
// RUN:   | FileCheck --check-prefix=KCFI-OPTS %s \
// RUN:       --implicit-check-not=error: --implicit-check-not=warning:
// KCFI-HASH: error: unsupported option '-fsanitize-kcfi-hash=FNV-1a' for target 'x86_64-unknown-windows-itanium'
// KCFI-NORMALIZE: error: unsupported option '-fsanitize-cfi-icall-experimental-normalize-integers' for target 'x86_64-unknown-windows-itanium'
// KCFI-ARITY: error: unsupported option '-fsanitize-kcfi-arity' for target 'x86_64-unknown-windows-itanium'
// KCFI-OPTS: "-cc1"
// KCFI-OPTS-SAME: "-fsanitize-cfi-icall-generalize-pointers"

// Under LTO, CFI checks the virtual and non-virtual member calls of the
// classes LTO sees whole, and the indirect calls through function and member
// function pointers, beside KCFI. With no sanitizer runtime, a failed check
// can only trap.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=LTO-CFI %s --implicit-check-not=error:
// RUN: %clang -### --target=aarch64-unknown-windows-itanium -c %s -flto=thin \
// RUN:     -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=LTO-CFI %s --implicit-check-not=error:
// LTO-CFI:     "-cc1"
// LTO-CFI-DAG: "-fsanitize=cfi-icall,cfi-mfcall,cfi-nvcall,cfi-vcall,kcfi"
// LTO-CFI-DAG: "-fsanitize-trap=cfi-icall,cfi-mfcall,cfi-nvcall,cfi-vcall"
// LTO-CFI-DAG: "-fsanitize-system-ignorelist={{[^"]*}}cfi_ignorelist.txt"
// LTO-CFI-DAG: "-fsplit-lto-unit"
// LTO-CFI-DAG: "-fwhole-program-vtables"
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-LTO-CFI %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -fno-sanitize=cfi -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-CFI %s
// NO-LTO-CFI:     "-cc1"
// NO-LTO-CFI-NOT: "-fsanitize={{[^"]*}}cfi-
// NO-LTO-CFI-NOT: -fwhole-program-vtables
// NO-CFI:     "-cc1"
// NO-CFI-NOT: "-fsanitize={{[^"]*}}cfi-

// A toolchain built without upstream's ignorelist still compiles with the
// default CFI; asking for CFI needs the list.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -resource-dir=%S 2>&1 \
// RUN:   | FileCheck --check-prefix=LTO-CFI-NO-LIST %s --implicit-check-not=error:
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -fsanitize=cfi-vcall -resource-dir=%S 2>&1 \
// RUN:   | FileCheck --check-prefix=CFI-NO-LIST %s
// LTO-CFI-NO-LIST: "-fsanitize=cfi-icall,cfi-mfcall,cfi-nvcall,cfi-vcall,kcfi"
// CFI-NO-LIST: error: missing sanitizer ignorelist
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -fsanitize=cfi -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=CFI-KCFI %s --implicit-check-not=error:
// CFI-KCFI: "-fsanitize=cfi-derived-cast,cfi-icall,cfi-mfcall,cfi-unrelated-cast,cfi-nvcall,cfi-vcall,kcfi"
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -fno-sanitize-trap=cfi-vcall -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=CFI-NO-TRAP %s
// CFI-NO-TRAP: error: unsupported option '-fno-sanitize-trap=cfi-vcall' for target 'x86_64-unknown-windows-itanium'

// So must every other check that would report through a runtime, and
// cross-DSO CFI, which calls the runtime for a target outside the image, is
// rejected.
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize=undefined 2>&1 \
// RUN:   | FileCheck --check-prefix=UBSAN-NO-TRAP %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s \
// RUN:     -fsanitize=undefined -fsanitize-trap=undefined 2>&1 \
// RUN:   | FileCheck --check-prefix=UBSAN-TRAP %s --implicit-check-not=error: \
// RUN:       --implicit-check-not=ubsan_standalone
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium -c %s -flto \
// RUN:     -fsanitize-cfi-cross-dso -resource-dir=%S/Inputs/resource_dir 2>&1 \
// RUN:   | FileCheck --check-prefix=CFI-CROSS-DSO %s
// UBSAN-NO-TRAP: error: unsupported option '-fsanitize=undefined' for target 'x86_64-unknown-windows-itanium'
// UBSAN-TRAP: "-fsanitize-trap=
// CFI-CROSS-DSO: error: unsupported option '-fsanitize-cfi-cross-dso' for target 'x86_64-unknown-windows-itanium'

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

// Control Flow Guard is on by default, and an executable suppresses its
// exports as call targets. On x86-64, which runs with the shadow stack, the
// image has the table of EH continuation targets.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CF,CF-X64 %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -mguard=cf 2>&1 \
// RUN:   | FileCheck --check-prefixes=CF,CF-A64 %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /guard:cf- \
// RUN:     /guard:cf -- %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=CF,CF-X64 %s
// CF:          "-cc1"
// CF-SAME:     "-cfguard"
// CF-NEXT:     lld-link{{(.exe)?}}"
// CF-X64-SAME: "-guard:cf,ehcont,exportsuppress"
// CF-A64-SAME: "-guard:cf,exportsuppress"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -shared 2>&1 \
// RUN:   | FileCheck --check-prefix=CF-DLL %s
// CF-DLL:      "-cc1"
// CF-DLL-SAME: "-cfguard"
// CF-DLL-NEXT: lld-link{{(.exe)?}}"
// CF-DLL-SAME: "-guard:cf,ehcont"
// CF-DLL-NOT:  "-guard:

// -mguard=none and /guard:cf- turn it off. The table of EH continuation
// targets stays.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -mguard=none 2>&1 \
// RUN:   | FileCheck --check-prefixes=NO-CF,NO-CF-X64 %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /guard:cf- \
// RUN:     -- %s 2>&1 \
// RUN:   | FileCheck --check-prefixes=NO-CF,NO-CF-X64 %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -mguard=none 2>&1 \
// RUN:   | FileCheck --check-prefixes=NO-CF,NO-CF-A64 %s
// NO-CF:          "-cc1"
// NO-CF-NOT:      "-cfguard"
// NO-CF-NOT:      "-cfguard-no-checks"
// NO-CF-NEXT:     lld-link{{(.exe)?}}"
// NO-CF-X64-SAME: "-guard:ehcont,nocf"
// NO-CF-A64-NOT:  "-guard:

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -mguard=cf-nochecks 2>&1 \
// RUN:   | FileCheck --check-prefix=CF-NOCHECKS %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium \
// RUN:     /guard:cf,nochecks -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CF-NOCHECKS %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /guard:cf \
// RUN:     /d2guardnochecks -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CF-NOCHECKS %s
// CF-NOCHECKS:      "-cc1"
// CF-NOCHECKS-NOT:  "-cfguard"
// CF-NOCHECKS-SAME: "-cfguard-no-checks"
// CF-NOCHECKS-NOT:  "-cfguard"
// CF-NOCHECKS-NEXT: lld-link{{(.exe)?}}"
// CF-NOCHECKS-SAME: "-guard:cf,ehcont,exportsuppress"

// /guard:ehcont and /guard:ehcont- leave the mode alone.
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /guard:ehcont- \
// RUN:     /c -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-EHCONT %s
// CL-EHCONT: "-cc1"
// CL-EHCONT-SAME: "-cfguard"

// RUN: not %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -mguard=ehcont 2>&1 \
// RUN:   | FileCheck --check-prefix=CF-UNKNOWN %s
// CF-UNKNOWN: error: unsupported argument 'ehcont' to option '-mguard='
// RUN: not %clang_cl -### --target=x86_64-unknown-windows-itanium \
// RUN:     /guard:foo /c -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CL-UNKNOWN %s
// CL-UNKNOWN: error: invalid value 'foo' in '/guard:'

//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
//--- include/c++/v1/__config
#define _LIBCPP_VERSION 230000
