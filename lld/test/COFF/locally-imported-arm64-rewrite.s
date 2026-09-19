// REQUIRES: aarch64

// An adrp/ldr pair that loads the import pointer of a symbol defined in the
// image becomes adrp/add of the symbol itself, and no pointer is emitted.
// The two instructions carry separate relocations that must agree, so a
// symbol with any reference in another form keeps the pointer for all of
// them.

// RUN: split-file %s %t.dir
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows %t.dir/pair.s -o %t.pair.obj
// RUN: llvm-mc -filetype=obj -triple=aarch64-windows %t.dir/mixed.s -o %t.mixed.obj

// RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t.exe %t.pair.obj 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
// RUN: llvm-objdump -d --no-show-raw-insn %t.exe | FileCheck %s
// RUN: llvm-readobj --sections --coff-basereloc %t.exe | FileCheck --check-prefix=NOPTR %s

// QUIET-NOT: locally defined symbol imported

// CHECK:      <main>:
// CHECK-NEXT:   adrp x8, 0x140001000 <main>
// CHECK-NEXT:   add x8, x8, #0x10
// CHECK-NEXT:   br x8
// CHECK:      <myfunc>:
// CHECK-NEXT:   ret

// NOPTR-NOT: Name: .rdata
// NOPTR:      BaseReloc [
// NOPTR-NEXT: ]

// RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t2.exe %t.mixed.obj 2>&1 | FileCheck --check-prefix=WARN %s
// RUN: llvm-objdump -d --no-show-raw-insn %t2.exe | FileCheck --check-prefix=MIXED %s

// WARN: warning: {{.*}}mixed.obj: locally defined symbol imported: myfunc (defined in {{.*}}mixed.obj) [LNK4217]

// MIXED:      <main>:
// MIXED-NEXT:   adrp x8, 0x140002000
// MIXED-NEXT:   ldr x8, [x8, #0x{{[0-9a-f]+}}]
// MIXED-NEXT:   br x8

#--- pair.s
    .text
    .globl main
    .globl myfunc
main:
    adrp x8, __imp_myfunc
    ldr  x8, [x8, :lo12:__imp_myfunc]
    br   x8
    ret
    .p2align 4
myfunc:
    ret

#--- mixed.s
    .text
    .globl main
    .globl myfunc
main:
    adrp x8, __imp_myfunc
    ldr  x8, [x8, :lo12:__imp_myfunc]
    br   x8
    ret
    .p2align 4
myfunc:
    ret

    .section .rdata, "dr"
    .quad __imp_myfunc
