# REQUIRES: x86, aarch64

## In the ntposix model an image's thread-local place is
## __imp___tp_register$init, the first word of .sealed after the word that
## the fill sets, with the image's scale beside it. A DLL with thread-local
## data has it whether or not code names it, and its fill writes it with what
## ntposix.dll's __tp_register returns for the image, the first word of
## the fill's list, of kind 2; the records of its thread-locals name it. An
## executable's holds the executable's place from the start, a constant of
## .rdata, unless its template holds a word that the fill writes, which it
## then registers again through the word.
##
## __imp_X$tpoff, and __imp_X$guard$tpoff of X's guard, are words of .sealed
## that the fill writes with the offset of X, and of its guard, from the
## thread pointer: the place that the record which __imp_X$tls points at
## names, plus the record's word, and hold 0xF1F1F1F1F1F1F1F1 until then. The
## list names each by that pointer, an import's entry or a pointer to the
## image's own record, with kind 5, and by the record's word, 8 or 12.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-ntposix
# DEFINE: %{dll} = lld-link -machine:%{a} -dll -noentry fill.obj ntposix.lib
# DEFINE: %{exe} = lld-link -machine:%{a} -entry:main -subsystem:console \
# DEFINE:   fill.obj exe.obj ntposix.lib
# DEFINE: %{dump} = llvm-objdump -s -j .rdata -j .sealed
# DEFINE: %{run} = \
# DEFINE:   lld-link -machine:%{a} -def:np.def -out:ntposix.lib && \
# DEFINE:   lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc} %S/Inputs/fill-entry.s -o fill.obj && \
# DEFINE:   %{mc} exe.s -o exe.obj && \
# DEFINE:   %{mc} tls.s -o tls.obj && %{mc} word.s -o word.obj && \
# DEFINE:   %{mc} use-%{t}.s -o use.obj && %{mc} main.s -o main.obj && \
# DEFINE:   %{dll} tls.obj -export:x$tls,data -out:d.dll -implib:d.lib && \
# DEFINE:   %{dump} d.dll | FileCheck --check-prefix=DLL %s && \
# DEFINE:   %{dll} use.obj d.lib -out:u.dll && \
# DEFINE:   %{dump} u.dll | FileCheck --check-prefix=USE %s && \
# DEFINE:   %{exe} main.obj tls.obj use.obj -export:x$tls,data -out:e.exe && \
# DEFINE:   %{dump} e.exe | FileCheck --check-prefix=EXE %s && \
# DEFINE:   %{exe} main.obj tls.obj word.obj imp.lib -out:w.exe && \
# DEFINE:   %{dump} w.exe | FileCheck --check-prefix=WORD %s && \
# DEFINE:   not lld-link -machine:%{a} -dll -noentry fill.obj tls.obj \
# DEFINE:     -export:x$tls,data -out:none.dll 2>&1 | FileCheck --check-prefix=NONE %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## A DLL has its list's one word, the place at 0x3008, from __tp_register's
## entry at 0x2000, kind 2; the record of x names the place, then x's
## offset, 4, and its guard's, 12; .sealed's done word, the place, which
## holds 0xF1F1F1F1F1F1F1F1 until the fill writes it, and the scale, 1.
# DLL:      Contents of section .rdata:
# DLL-NEXT: 180002000 c8200000 00000000 00000000 00000000
# DLL-NEXT: 180002010 00000000 00000000 01000000 00000000
# DLL-NEXT: 180002020 00300000 18000000 00000000 00000000
# DLL-NEXT: 180002030 08300000 02200000 00000000 00000000
# DLL-NEXT: 180002040 08300080 01000000 04000000 0c000000
# DLL:      Contents of section .sealed:
# DLL-NEXT: 180003000 00000000 00000000 f1f1f1f1 f1f1f1f1
# DLL-NEXT: 180003010 01000000 00000000

## The importer: its guard's offset at 0x3008 and x's at 0x3010, each from
## the entry of x$tls at 0x2000, kind 5, and the record's word, 12 or 8;
## each holds 0xF1F1F1F1F1F1F1F1 until the fill writes it.
# USE:      Contents of section .rdata:
# USE-NEXT: 180002000 88200000 00000000 00000000 00000000
# USE-NEXT: 180002010 00000000 00000000 02000000 00000000
# USE-NEXT: 180002020 00300000 18000000 00000000 00000000
# USE-NEXT: 180002030 08300000 05200000 0c000000 00000000
# USE-NEXT: 180002040 10300000 05200000 08000000 00000000
# USE:      Contents of section .sealed:
# USE-NEXT: 180003000 00000000 00000000 f1f1f1f1 f1f1f1f1
# USE-NEXT: 180003010 f1f1f1f1 f1f1f1f1{{ }}

## The executable: its words are computed from the pointer at 0x2040 to its
## record, which names its place, 0x100, and scale, constants of .rdata.
# EXE:      Contents of section .rdata:
# EXE-NEXT: 140002000 00000000 00000000 02000000 00000000
# EXE-NEXT: 140002010 00300000 18000000 00000000 00000000
# EXE-NEXT: 140002020 08300000 45200000 0c000000 00000000
# EXE-NEXT: 140002030 10300000 45200000 08000000 00000000
# EXE-NEXT: 140002040 48200040 01000000 58200040 01000000
# EXE-NEXT: 140002050 04000000 0c000000 00010000 00000000
# EXE-NEXT: 140002060 01000000 00000000

## An executable whose template holds a word that the fill writes registers
## again: its place, first in .sealed, holds 0x100 from the start, and the
## list names it, kind 2, before the word of the template.
# WORD:      Contents of section .rdata:
# WORD:      140002020 01000000 00000000 02000000 00000000
# WORD-NEXT: 140002030 00300000 18000000 00000000 00000000
# WORD-NEXT: 140002040 10400000 08000000 08300000 12200000
# WORD-NEXT: 140002050 00000000 00000000 10400000 00200000
# WORD:      Contents of section .sealed:
# WORD-NEXT: 140003000 00000000 00000000 00010000 00000000
# WORD-NEXT: 140003010 01000000 00000000

# NONE: error: the image's thread-locals need its place, __imp___tp_register$init, which ntposix.dll's __tp_register gives; link ntposix.lib

#--- np.def
LIBRARY ntposix.dll
EXPORTS
__tp_register

#--- imp.def
LIBRARY imp.dll
EXPORTS
obj DATA

#--- tls.s
        .section .tls$,"dw"
        .globl  x, y
        .long   0
x:      .long   5
y:      .long   6
guard:  .byte   0
        .section .tls$,"dw",discard,__tls_set
        .globl  __tls_set
__tls_set:
        .byte   1
        .link_fact x, 7, guard
        .link_fact y, 7, __tls_set

#--- word.s
        .section .tls$a,"dw"
        .p2align 3
        .quad   obj+8

#--- exe.s
        .section .text,"xr",one_only,__llvm_fill_exe
        .globl  __llvm_fill_exe
__llvm_fill_exe:
        ret
        .rva    __llvm_fill_list
        .rva    __llvm_fill_target

#--- main.s
        .text
        .globl  main
main:
        ret

#--- use-x86_64.s
        .text
        .globl  use
use:
        movq    __imp_x$tpoff(%rip), %rax
        movl    (%r14,%rax), %eax
        movq    __imp_x$guard$tpoff(%rip), %rcx
        retq

#--- use-aarch64.s
        .text
        .globl  use
use:
        adrp    x8, __imp_x$tpoff
        ldr     x8, [x8, :lo12:__imp_x$tpoff]
        ldr     w0, [x28, x8]
        adrp    x9, __imp_x$guard$tpoff
        ldr     x9, [x9, :lo12:__imp_x$guard$tpoff]
        ret
