# REQUIRES: x86, aarch64

## An importer reaches another image's thread-local through a word of its own,
## and its guard through a second, where the import library that offers X$tls
## offers __tp_unit, which an image whose form is direct exports: the chain of
## the record becomes each word's load and an access at the thread pointer
## plus the word, ELF's initial-exec, the thread pointer in the register the
## chain's base adds the place to, and the rest of the chain is dead, deleted
## where the assembler kept no size and else no-operations. Where an access
## does not say that it reads the thread pointer there, the chain's base is
## the thread pointer instead and its accesses add the words to it. The words are a pair of .sealed that the fill writes
## from the record, kind 5, and hold 0xF1F1F1F1F1F1F1F1 until then. A wide
## image's import library offers no unit, and its importer keeps the record.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-ntposix
# DEFINE: %{dll} = lld-link -machine:%{a} -dll -noentry fill.obj ntposix.lib
# DEFINE: %{run} = \
# DEFINE:   lld-link -machine:%{a} -def:np.def -out:ntposix.lib && \
# DEFINE:   %{mc} %S/Inputs/fill-entry.s -o fill.obj && %{mc} tls.s -o tls.obj && \
# DEFINE:   %{mc} big.s -o big.obj && %{mc} %S/Inputs/tls-chain-%{t}.s -o use.obj && \
# DEFINE:   %{dll} tls.obj -export:x$tls,data -out:d.dll -implib:d.lib && \
# DEFINE:   %{dll} tls.obj big.obj -export:x$tls,data -out:w.dll -implib:w.lib && \
# DEFINE:   llvm-nm d.lib | FileCheck --check-prefix=UNIT %s && \
# DEFINE:   llvm-nm w.lib | FileCheck --check-prefix=NO-UNIT %s && \
# DEFINE:   %{dll} use.obj d.lib -out:u.dll -debug:symtab && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn u.dll | \
# DEFINE:     FileCheck --check-prefix=TPOFF-%{t} %s && \
# DEFINE:   llvm-objdump -s -j .rdata -j .sealed u.dll | FileCheck --check-prefix=WORDS %s && \
# DEFINE:   %{mc} %S/Inputs/tls-chain2-%{t}.s -o two.obj && \
# DEFINE:   %{dll} two.obj d.lib -out:two.dll -debug:symtab && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn two.dll | \
# DEFINE:     FileCheck --check-prefix=TWO-%{t} %s && \
# DEFINE:   sed '/site_fact 14,/d' %S/Inputs/tls-chain-%{t}.s | %{mc} -o base.obj && \
# DEFINE:   %{dll} base.obj d.lib -out:b.dll -debug:symtab && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn b.dll | \
# DEFINE:     FileCheck --check-prefix=BASE-%{t} %s && \
# DEFINE:   %{dll} use.obj w.lib -out:r.dll -debug:symtab && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn r.dll | \
# DEFINE:     FileCheck --check-prefix=RECORD-%{t} %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# UNIT:        D __imp___tp_unit
# UNIT:        D __imp_x$tls
# NO-UNIT-NOT: __tp_unit

## Each access adds its word to the thread pointer, r14 or x28, which the
## chain's base names.
# TPOFF-x86_64:      <use$tp>:
# TPOFF-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rcx # 0x180003018
# TPOFF-x86_64-NEXT: cmpb $0x0, (%r14,%rcx)
# TPOFF-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rax # 0x180003010
# TPOFF-x86_64-NEXT: movl (%r14,%rax), %eax
# TPOFF-x86_64-NEXT: retq

## The record's adrp gives the pair's page to both loads.
# TPOFF-aarch64:          <use$tp>:
# TPOFF-aarch64-NEXT:     adrp x8, 0x180003000
# TPOFF-aarch64-NEXT:     ldr x13, [x8, #0x18]
# TPOFF-aarch64-NEXT:     ldr x8, [x8, #0x10]
# TPOFF-aarch64-NEXT:     ldrb w14, [x28, x13]
# TPOFF-aarch64-NEXT:     ldr w0, [x28, x8]
# TPOFF-aarch64-NEXT:     ret

## Chains that share the load of the place word's address and the key's
## each load their word; the cold part's load of the record's pointer is
## dead, its ARM64 adrp giving the words' page.
# TWO-x86_64:      <two$tp>:
# TWO-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rcx # 0x180003018
# TWO-x86_64-NEXT: cmpb $0x0, (%r14,%rcx)
# TWO-x86_64-NEXT: je
# TWO-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rax # 0x180003010
# TWO-x86_64-NEXT: movl (%r14,%rax), %eax
# TWO-x86_64-NEXT: retq
# TWO-x86_64-NEXT: callq {{.*}} <init>
# TWO-x86_64-NEXT: jmp
# TWO-aarch64:      <two$tp>:
# TWO-aarch64-NEXT: adrp x8, 0x180003000
# TWO-aarch64-NEXT: ldr x13, [x8, #0x18]
# TWO-aarch64-NEXT: ldrb w14, [x28, x13]
# TWO-aarch64-NEXT: cbz
# TWO-aarch64-NEXT: ldr x8, [x8, #0x10]
# TWO-aarch64-NEXT: ldr w0, [x28, x8]
# TWO-aarch64-NEXT: ret
# TWO-aarch64-NEXT: bl {{.*}} <init>
# TWO-aarch64-NEXT: adrp x8, 0x180003000
# TWO-aarch64-NEXT: b

# BASE-x86_64:      <use$tp>:
# BASE-x86_64-NEXT: leaq (%r14), %rsi
# BASE-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rcx # 0x180003018
# BASE-x86_64-NEXT: cmpb $0x0, (%rsi,%rcx)
# BASE-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rax # 0x180003010
# BASE-x86_64-NEXT: movl (%rsi,%rax), %eax
# BASE-x86_64-NEXT: retq
# BASE-aarch64:      <use$tp>:
# BASE-aarch64-NEXT: adrp x8, 0x180003000
# BASE-aarch64-NEXT: ldr x13, [x8, #0x18]
# BASE-aarch64-NEXT: ldr x8, [x8, #0x10]
# BASE-aarch64-NEXT: add x10, x28, #0x0
# BASE-aarch64-NEXT: mov x9, x10
# BASE-aarch64-NEXT: ldrb w14, [x9, x13]
# BASE-aarch64-NEXT: ldr w0, [x9, x8]
# BASE-aarch64-NEXT: ret

## The fill's list names x's word at 0x3010 and its guard's at 0x3018 by the
## entry of x$tls at 0x2000, kind 5, and the record's word, 8 or 12. No chain
## reads the key, whose import the image leaves out.
# WORDS:      Contents of section .rdata:
# WORDS:      180002030 10300000 05200000 08000000 00000000
# WORDS-NEXT: 180002040 18300000 05200000 0c000000 00000000
# WORDS:      Contents of section .sealed:
# WORDS-NEXT: 180003000 00000000 00000000 00000000 00000000
# WORDS-NEXT: 180003010 f1f1f1f1 f1f1f1f1 f1f1f1f1 f1f1f1f1

# RECORD-x86_64:      <use$tp>:
# RECORD-x86_64-NEXT: movq {{.*}}(%rip), %rax # {{.*}} <__imp_x$tls>
# RECORD-x86_64-NEXT: movq (%rax), %rcx
# RECORD-x86_64:      cmovneq %rdx, %rsi
# RECORD-aarch64:      <use$tp>:
# RECORD-aarch64-NEXT: ldr x8, {{.*}} <__imp_x$tls>
# RECORD-aarch64-NEXT: ldr x9, [x8]
# RECORD-aarch64:      csel x9, x11, x10, ne

#--- np.def
LIBRARY ntposix.dll
EXPORTS
__tp_register
__tp_key DATA

#--- tls.s
        .section .tls$,"dw"
        .globl  x
        .long   0
x:      .long   5
guard:  .byte   0
        .link_fact x, 7, guard

#--- big.s
        .section .tls$z,"dw"
        .zero   2048
