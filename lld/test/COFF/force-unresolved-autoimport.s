# REQUIRES: x86

## Under -force:unresolved, a symbol whose __imp_ symbol is also unresolved is
## reported, not automatically imported from the placeholder that replaces
## the __imp_ symbol, whichever of the two the linker visits first.

# RUN: llvm-mc -triple=x86_64-windows-gnu %s -filetype=obj -o %t.obj
# RUN: lld-link -lldmingw -entry:main -out:%t.exe %t.obj -force:unresolved 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not="unable to automatically import"

# CHECK-DAG: warning: undefined symbol: aaa
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) aaa
# CHECK-DAG: warning: undefined symbol: bbb
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) bbb
# CHECK-DAG: warning: undefined symbol: ccc
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) ccc
# CHECK-DAG: warning: undefined symbol: ddd
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) ddd
# CHECK-DAG: warning: undefined symbol: eee
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) eee
# CHECK-DAG: warning: undefined symbol: fff
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) fff
# CHECK-DAG: warning: undefined symbol: ggg
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) ggg
# CHECK-DAG: warning: undefined symbol: hhh
# CHECK-DAG: warning: undefined symbol: __declspec(dllimport) hhh

    .globl main
    .text
main:
    movl aaa(%rip), %eax
    movq __imp_aaa(%rip), %rax
    movl bbb(%rip), %eax
    movq __imp_bbb(%rip), %rax
    movl ccc(%rip), %eax
    movq __imp_ccc(%rip), %rax
    movl ddd(%rip), %eax
    movq __imp_ddd(%rip), %rax
    movl eee(%rip), %eax
    movq __imp_eee(%rip), %rax
    movl fff(%rip), %eax
    movq __imp_fff(%rip), %rax
    movl ggg(%rip), %eax
    movq __imp_ggg(%rip), %rax
    movl hhh(%rip), %eax
    movq __imp_hhh(%rip), %rax
    ret
