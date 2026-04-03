// REQUIRES: x86-registered-target
// REQUIRES: win32-itanium-default-libc-llvm-libc

// RUN: rm -rf %t.libc && mkdir -p %t.libc/bin %t.libc/include/x86_64-unknown-windows-itanium %t.libc/lib/x86_64-unknown-windows-itanium
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/crt1.obj
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/dllcrt.obj
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/crt_tls.obj
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/crt_gs.obj
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/crt_cfg.obj
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/crt_loadcfg.obj
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/crt_delayload.obj
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/c.lib
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/kernelbase.lib
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/ntdll.lib
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/sspicli.lib
// RUN: touch %t.libc/lib/x86_64-unknown-windows-itanium/bcryptprimitives.lib

// RUN: %clang --target=x86_64-unknown-windows-itanium -ccc-install-dir %t.libc/bin -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=INCLUDES %s
// INCLUDES: "-internal-isystem" "{{.*}}include{{/|\\\\}}x86_64-unknown-windows-itanium"
// INCLUDES-NOT: "{{.*}}Include{{/|\\\\}}{{.*}}ucrt"

// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -ccc-install-dir %t.libc/bin -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK %s
// LINK: lld-link
// LINK: "{{.*}}crt1.obj"
// LINK: "{{.*}}crt_tls.obj"
// LINK: "{{.*}}crt_gs.obj"
// LINK: "{{.*}}crt_cfg.obj"
// LINK: "{{.*}}crt_loadcfg.obj"
// LINK: "{{.*}}c.lib"
// LINK: "{{.*}}kernelbase.lib"
// LINK: "{{.*}}ntdll.lib"
// LINK: "{{.*}}sspicli.lib"
// LINK: "{{.*}}bcryptprimitives.lib"
// LINK: "kernel32.lib"
// LINK: "bcrypt.lib"
// LINK: "clang_rt.builtins-x86_64.lib"
// LINK-NOT: "ucrt.lib"
// LINK-NOT: "vcruntime.lib"
// LINK-NOT: "wincrt"

// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -nostartfiles -ccc-install-dir %t.libc/bin -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTARTFILES %s
// NOSTARTFILES-NOT: "{{.*}}crt1.obj"
// NOSTARTFILES-NOT: "{{.*}}crt_tls.obj"
// NOSTARTFILES: "{{.*}}c.lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -nolibc -ccc-install-dir %t.libc/bin -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOLIBC %s
// NOLIBC-NOT: "{{.*}}c.lib"
// NOLIBC-NOT: "{{.*}}kernelbase.lib"
// NOLIBC-NOT: "{{.*}}ntdll.lib"
// NOLIBC-NOT: "kernel32.lib"
// NOLIBC: "{{.*}}crt1.obj"

// RUN: %clang --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -Wl,/delayload:foo.dll -ccc-install-dir %t.libc/bin -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DELAYLOAD %s
// DELAYLOAD: "{{.*}}crt_delayload.obj"

// RUN: not %clang --target=x86_64-unknown-windows-itanium -rtlib=platform \
// RUN:   -ccc-install-dir %t.libc/bin -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=RTLIBERR %s
// RTLIBERR: error: unsupported runtime library 'platform' for platform 'x86_64-unknown-windows-itanium'

// RUN: rm -rf %t.missing && mkdir -p %t.missing/bin %t.missing/lib/x86_64-unknown-windows-itanium
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/crt_tls.obj
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/crt_gs.obj
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/crt_cfg.obj
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/crt_loadcfg.obj
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/c.lib
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/kernelbase.lib
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/ntdll.lib
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/sspicli.lib
// RUN: touch %t.missing/lib/x86_64-unknown-windows-itanium/bcryptprimitives.lib
// RUN: not %clang --target=x86_64-unknown-windows-itanium -rtlib=compiler-rt \
// RUN:   -ccc-install-dir %t.missing/bin -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=MISSING %s
// MISSING: error: no such file or directory: 'crt1.obj'

int main(void) { return 0; }
