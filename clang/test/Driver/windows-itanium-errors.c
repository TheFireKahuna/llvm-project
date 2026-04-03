// REQUIRES: x86-registered-target

// Test error and warning diagnostics for Windows Itanium toolchain.

// RUN: %clang --target=x86_64-unknown-windows-itanium -fdwarf-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DWARF_WARN %s

// DWARF_WARN: warning: ignoring '-fdwarf-exceptions' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// DWARF_WARN: "-exception-model=seh"
// DWARF_WARN-NOT: "-exception-model=dwarf"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fwasm-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=WASM_WARN %s

// WASM_WARN: warning: ignoring '-fwasm-exceptions' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// WASM_WARN: "-exception-model=seh"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fseh-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SEH_OK %s

// SEH_OK-NOT: warning:
// SEH_OK: "-exception-model=seh"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fsjlj-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SJLJ_OK %s

// SJLJ_OK-NOT: warning:
// SJLJ_OK: "-exception-model=sjlj"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fuse-ld=link -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LINK_EXE_WARN %s

// LINK_EXE_WARN: warning: ignoring '-fuse-ld=link' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'

// RUN: not %clang --target=x86_64-unknown-windows-itanium -mguard=foo -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MGUARD_INVALID %s

// MGUARD_INVALID: error: unsupported argument 'foo' to option '-mguard='

// RUN: not %clang --target=x86_64-unknown-windows-itanium -fsanitize=thread -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=TSAN_ERR %s

// TSAN_ERR: error: unsupported option '-fsanitize=thread' for target 'x86_64-unknown-windows-itanium'

// RUN: not %clang --target=x86_64-unknown-windows-itanium -fsanitize=memory -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MSAN_ERR %s

// MSAN_ERR: error: unsupported option '-fsanitize=memory' for target 'x86_64-unknown-windows-itanium'

// RUN: %clang --target=i686-unknown-windows-itanium -fseh-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SEH_OK_X86 %s

// SEH_OK_X86-NOT: warning:
// SEH_OK_X86: "-exception-model=seh"

// RUN: %clang --target=aarch64-unknown-windows-itanium -fseh-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SEH_OK_ARM64 %s

// SEH_OK_ARM64-NOT: warning:
// SEH_OK_ARM64: "-exception-model=seh"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -stdlib=libstdc++ -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBSTDCXX %s

// LIBSTDCXX: warning: argument unused during compilation: '-stdlib=libstdc++'
// LIBSTDCXX: "-defaultlib:c++"
