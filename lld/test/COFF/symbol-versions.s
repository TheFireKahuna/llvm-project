# REQUIRES: x86
## A symbol whose name carries a version, as .symver gives it, is exported
## under "name@version". A default version, "name@@version", is also exported
## as plain name, which it defines in the image, and the import library binds
## a reference to name to the default version's export through EXPORTAS, so
## that a binary keeps the version it was linked against. A named node of a
## version script gives the exports whose names it lists a default version.
## A Microsoft C++ name and a vectorcall name carry no version.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj

# RUN: lld-link -dll -noentry lib.obj -out:lib.dll -implib:lib.lib \
# RUN:   -version-script:lib.map
# RUN: llvm-readobj --coff-exports lib.dll | FileCheck --check-prefix=EXPORTS %s
# EXPORTS:      Name: ?f@@YAXXZ
# EXPORTS-NEXT: RVA:
# EXPORTS:      Name: bar
# EXPORTS:      Name: bar@LIB_2
# EXPORTS:      Name: foo
# EXPORTS:      Name: foo@LIB_1
# EXPORTS:      Name: foo@LIB_2
# EXPORTS:      Name: vc@@16
# EXPORTS-NOT:  Name:

# RUN: llvm-readobj lib.lib | FileCheck --check-prefix=IMPLIB %s
# IMPLIB:      Name type: export as
# IMPLIB-NEXT: Export name: bar@LIB_2
# IMPLIB-NEXT: Symbol: __imp_bar
# IMPLIB:      Name type: export as
# IMPLIB-NEXT: Export name: foo@LIB_2
# IMPLIB-NEXT: Symbol: __imp_foo

## The image's own reference to foo binds the default version.
# RUN: llvm-objdump -d lib.dll | FileCheck --check-prefix=LOCAL %s
# LOCAL:      <foo@LIB_2>:
# LOCAL-NEXT: movl $0x2, %eax
# LOCAL:      jmp {{.*}} <foo@LIB_2>

## An executable linked against the import library imports the versions.
# RUN: lld-link -entry:main -subsystem:console use.obj lib.lib -out:use.exe
# RUN: llvm-readobj --coff-imports use.exe | FileCheck --check-prefix=IMPORTS %s
# IMPORTS:      Name: lib.dll
# IMPORTS:      Symbol: bar@LIB_2
# IMPORTS-NEXT: Symbol: foo@LIB_2

#--- lib.s
        .text
        .globl foo_v1, foo_v2, bar, caller, "?f@@YAXXZ", "vc@@16"
foo_v1:
        movl $1, %eax
        retq
foo_v2:
        movl $2, %eax
        retq
bar:
        movl $3, %eax
        retq
caller:
        jmp foo
"?f@@YAXXZ":
"vc@@16":
        retq
        .symver foo_v1, foo@LIB_1
        .symver foo_v2, foo@@LIB_2
        .section .drectve,"yni"
        .ascii " -export:bar -export:?f@@YAXXZ -export:vc@@16"

#--- lib.map
LIB_1 { };
LIB_2 { global: bar; } LIB_1;

#--- use.s
        .text
        .globl main
main:
        callq *__imp_foo(%rip)
        callq *__imp_bar(%rip)
        retq
