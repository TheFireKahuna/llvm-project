# REQUIRES: x86
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link -safeseh:no /dll /noentry /winsysroot:%t.dir/sysroot /vctoolsversion:1.1.1.1 /winsdkversion:10.0.1 /libpath:custom-dir %t.obj -print-search-paths | FileCheck -DSYSROOT=%t.dir %s
# RUN: llvm-mc -triple i686-windows-msvc %s -filetype=obj -o %t_32.obj
# RUN: lld-link -safeseh:no /dll /noentry /winsysroot:%t.dir/sysroot /vctoolsversion:1.1.1.1 /winsdkversion:10.0.1 %t_32.obj -print-search-paths | FileCheck -DSYSROOT=%t.dir -check-prefix=X86 %s
# CHECK: Library search paths:
# CHECK-NEXT:   (cwd)
# CHECK-NEXT:   custom-dir
# CHECK-NEXT:   [[CPATH:.*]]lib{{[/\\]}}clang{{[/\\]}}{{[0-9]+}}{{[/\\]}}lib{{[/\\]}}windows
# CHECK-NEXT:   [[CPATH]]lib{{[/\\]}}clang{{[/\\]}}{{[0-9]+}}{{[/\\]}}lib
# CHECK-NEXT:   [[CPATH]]lib
# CHECK-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}DIA SDK{{[/\\]}}lib{{[/\\]}}amd64
# CHECK-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}VC{{[/\\]}}Tools{{[/\\]}}MSVC{{[/\\]}}1.1.1.1{{[/\\]}}lib{{[/\\]}}x64
# CHECK-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}VC{{[/\\]}}Tools{{[/\\]}}MSVC{{[/\\]}}1.1.1.1{{[/\\]}}atlmfc{{[/\\]}}lib{{[/\\]}}x64
# CHECK-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}Windows Kits{{[/\\]}}10{{[/\\]}}Lib{{[/\\]}}10.0.1{{[/\\]}}ucrt{{[/\\]}}x64
# CHECK-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}Windows Kits{{[/\\]}}10{{[/\\]}}Lib{{[/\\]}}10.0.1{{[/\\]}}um{{[/\\]}}x64
# X86: Library search paths:
# X86-NEXT:   (cwd)
# X86-NEXT:   [[CPATH:.*]]lib{{[/\\]}}clang{{[/\\]}}{{[0-9]+}}{{[/\\]}}lib{{[/\\]}}windows
# X86-NEXT:   [[CPATH]]lib{{[/\\]}}clang{{[/\\]}}{{[0-9]+}}{{[/\\]}}lib
# X86-NEXT:   [[CPATH]]lib
# X86-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}DIA SDK{{[/\\]}}lib
# X86-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}VC{{[/\\]}}Tools{{[/\\]}}MSVC{{[/\\]}}1.1.1.1{{[/\\]}}lib{{[/\\]}}x86
# X86-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}VC{{[/\\]}}Tools{{[/\\]}}MSVC{{[/\\]}}1.1.1.1{{[/\\]}}atlmfc{{[/\\]}}lib{{[/\\]}}x86
# X86-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}Windows Kits{{[/\\]}}10{{[/\\]}}Lib{{[/\\]}}10.0.1{{[/\\]}}ucrt{{[/\\]}}x86
# X86-NEXT:   [[SYSROOT]]{{[/\\]}}sysroot{{[/\\]}}Windows Kits{{[/\\]}}10{{[/\\]}}Lib{{[/\\]}}10.0.1{{[/\\]}}um{{[/\\]}}x86

## Without /winsysroot, /vctoolsdir or /winsdkdir, the MSVC and Windows SDK
## directories are looked for only once a search misses the other paths.
# RUN: mkdir -p %t.dir/vc/lib/x64 %t.dir/lib
# RUN: cp %p/Inputs/std64.lib %t.dir/vc/lib/x64
# RUN: cp %p/Inputs/std64.lib %t.dir/lib
# RUN: env -u LIB VCToolsInstallDir=%t.dir/vc lld-link %p/Inputs/hello64.obj \
# RUN:   /libpath:%t.dir/lib std64.lib /entry:main /out:%t.exe \
# RUN:   -print-search-paths | FileCheck --check-prefix=FOUND %s
# RUN: env -u LIB VCToolsInstallDir=%t.dir/vc lld-link %p/Inputs/hello64.obj \
# RUN:   std64.lib /entry:main /out:%t.exe -print-search-paths \
# RUN:   | FileCheck --check-prefix=MISS %s
# FOUND:     Library search paths:
# FOUND-NOT: {{[/\\]}}vc{{[/\\]}}lib
# MISS:      Library search paths:
# MISS:      {{[/\\]}}vc{{[/\\]}}lib{{[/\\]}}x64


        .text
        .globl  _main
_main:
        ret
