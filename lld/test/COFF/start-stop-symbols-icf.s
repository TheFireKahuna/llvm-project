# REQUIRES: x86

## A section of a run that __start_X and __stop_X bound is not folded by ICF,
## which would drop it from the run. ICF folds only sections of the same name,
## so the case is two identical sections of the run.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/main.s -filetype=obj -o %t.main.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/bounds.s -filetype=obj -o %t.bounds.obj

## Without the bounds, f1 and f2 fold.
# RUN: lld-link %t.main.obj -entry:main -opt:icf -debug:symtab -out:%t.exe
# RUN: llvm-objdump -t %t.exe | FileCheck %s --check-prefix=FOLDED
# FOLDED: (sec  2){{.*}} 0x[[F:[0-9a-f]+]] f1
# FOLDED: (sec  2){{.*}} 0x[[F]] f2

## With them, each keeps its own section.
# RUN: lld-link %t.main.obj %t.bounds.obj -entry:main -opt:icf \
# RUN:   -start-stop-symbols -debug:symtab -out:%t.bounds.exe
# RUN: llvm-objdump -t %t.bounds.exe | FileCheck %s --check-prefix=KEPT
# KEPT: (sec  3){{.*}} 0x00000000 f1
# KEPT: (sec  3){{.*}} 0x00000006 f2

#--- main.s
        .globl main
        .text
main:
        callq f1
        callq f2
        retq

        .section fns,"xr",discard,f1
        .globl f1
f1:
        movl $1, %eax
        retq

        .section fns,"xr",discard,f2
        .globl f2
f2:
        movl $1, %eax
        retq

#--- bounds.s
        .data
        .quad __start_fns
        .quad __stop_fns
