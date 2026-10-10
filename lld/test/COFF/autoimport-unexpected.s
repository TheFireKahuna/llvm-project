# REQUIRES: x86

## Automatic import of a symbol whose __imp_ symbol is neither an import nor a
## regular definition is reported, not imported, whatever defines it: here an
## absolute symbol, as -force:unresolved also makes of an unresolved __imp_.

# RUN: llvm-mc -triple=x86_64-windows-gnu %s -filetype=obj -o %t.obj
# RUN: not lld-link -lldmingw -entry:main -out:%t.exe %t.obj 2>&1 \
# RUN:   | FileCheck %s

# CHECK: warning: unable to automatically import foo from __imp_foo from <internal>; unexpected symbol type
# CHECK: error: undefined symbol: foo

    .globl main
    .text
main:
    movl foo(%rip), %eax
    ret

    .globl __imp_foo
__imp_foo = 0x1000
