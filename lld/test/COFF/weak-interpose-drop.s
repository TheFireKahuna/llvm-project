# REQUIRES: x86

# An executable is final: nothing it loads supersedes a definition it holds,
# so the entry a weak definition carries in front of its body can never take
# its branch there, and it is replaced with padding. The entry comes two ways
# round, because a body that is a tail call becomes the branch's own target;
# that one keeps the branch and makes it unconditional, which an unconditional
# jump ending where the conditional one ended does without moving the
# relocated displacement. A library keeps both, since that is where the
# mechanism does its work.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium %t.dir/main.s -o %t.obj

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t.exe %t.obj
# RUN: llvm-objdump -d %t.exe | FileCheck --check-prefix=EXE %s

# RUN: lld-link -dll -noentry -out:%t.dll %t.obj -export:fallthrough -export:tailcall
# RUN: llvm-objdump -d %t.dll | FileCheck --check-prefix=DLL %s

# The body falls through, so the whole entry is padding.
# EXE-LABEL: <fallthrough>:
# EXE-NEXT:    nopw
# EXE-NEXT:    nop
# EXE-NEXT:    retq
# The forward is left where it was, unreachable.
# EXE-NEXT:    jmpq *%rax

# The body is the branch's target, so the branch stays and loses its
# condition.
# EXE-LABEL: <tailcall>:
# EXE-NEXT:    nopw
# EXE-NEXT:    jmp
# EXE-NEXT:    jmpq *%rax

# Neither keeps the load or the test.
# EXE-NOT:     testq %rax, %rax

# A library keeps the entry as the compiler emitted it.
# DLL-LABEL: <fallthrough>:
# DLL-NEXT:    movq
# DLL-NEXT:    testq %rax, %rax
# DLL-NEXT:    jne
# DLL-LABEL: <tailcall>:
# DLL-NEXT:    movq
# DLL-NEXT:    testq %rax, %rax
# DLL-NEXT:    je

#--- main.s
        .text
        .globl  main
main:
# Naming the bounds is what says this image's startup code binds the records.
        movq    __wkintp_start(%rip), %rax
        movq    __wkintp_end(%rip), %rax
        xorl    %eax, %eax
        retq

        .globl  fallthrough
        .p2align 4
fallthrough:
        movq    interpose(%rip), %rax
        testq   %rax, %rax
        jne     .Lforward
        retq
.Lforward:
        jmpq    *%rax

        .globl  tailcall
        .p2align 4
tailcall:
        movq    interpose+32(%rip), %rax
        testq   %rax, %rax
        je      body
        jmpq    *%rax

        .section        .text$z,"xr"
        .globl  body
body:
        retq

        .section        .wkintp,"dr"
        .p2align 3
interpose:
        .zero   64
