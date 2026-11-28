# REQUIRES: x86, aarch64

## In the ntposix model an image's thread-local place is
## __imp___tp_register$init, the first word of .sealed after the word that
## the fill sets, {place, form, key}. A DLL with thread-local
## data has it whether or not code names it, and its fill writes it with what
## ntposix.dll's __tp_register returns for the image, the first word of
## the fill's list, of kind 2; the records of its thread-locals name it. An
## executable's holds the executable's place from the start, a constant of
## .rdata, unless its template holds a word that the fill writes, which it
## then registers again through the word.
##
## x@gottpoff, ELF's initial-exec, reads a word of .sealed that the link makes
## and the fill writes with the offset of x from the thread pointer: the place
## that the record x$tls names plus the record's offset, holding
## 0xF1F1F1F1F1F1F1F1 until then. The list names it by the record's pointer,
## an import's entry or a pointer to the image's own record, with kind 5, and
## by the record's word, 8. No object names the word. Where the link knows
## the image's place, it writes the offset into the code instead, as ELF
## relaxes initial-exec to local-exec. An image whose form is direct and which
## exports a record exports __tp_unit at its place word.

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
# DEFINE:   %{dll} tls.obj use.obj -export:x$tls,data -out:o.dll && \
# DEFINE:   %{dump} o.dll | FileCheck --check-prefix=OWN %s && \
# DEFINE:   %{exe} main.obj tls.obj use.obj -out:e.exe && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn e.exe | \
# DEFINE:     FileCheck --check-prefix=LE-%{a} %s && \
# DEFINE:   %{exe} main.obj tls.obj word.obj imp.lib -out:w.exe && \
# DEFINE:   %{dump} w.exe | FileCheck --check-prefix=WORD %s && \
# DEFINE:   not lld-link -machine:%{a} -dll -noentry fill.obj tls.obj \
# DEFINE:     -export:x$tls,data -out:none.dll 2>&1 | FileCheck --check-prefix=NONE %s && \
# DEFINE:   lld-link -machine:%{a} -def:wide.def -out:wide.lib && \
# DEFINE:   not %{dll} use.obj wide.lib -out:wide.dll 2>&1 | \
# DEFINE:     FileCheck --check-prefix=WIDE %s && \
# DEFINE:   %{mc} own-%{t}.s -o own.obj && \
# DEFINE:   not %{dll} own.obj -out:own.dll 2>&1 | FileCheck --check-prefix=NOREC %s && \
# DEFINE:   %{mc} named.s -o named.obj && \
# DEFINE:   not %{dll} named.obj d.lib -out:named.dll 2>&1 | \
# DEFINE:     FileCheck --check-prefix=NAMED %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## A DLL has its list's one word, the place at 0x3008, from __tp_register's
## entry at 0x2000, kind 2; the record of x names the place, then x's
## offset, 4, and its guard's, 12; .sealed's done word, the place, which
## holds 0xF1F1F1F1F1F1F1F1 until the fill writes it, the form, 0, direct, and
## the key, 0. __tp_unit is exported at 0x3008.
# DLL:      Contents of section .rdata:
# DLL-NEXT: 180002000 e0200000 00000000 00000000 00000000
# DLL-NEXT: 180002010 00000000 00000000 01000000 00000000
# DLL-NEXT: 180002020 00300000 20000000 00000000 00000000
# DLL-NEXT: 180002030 08300000 02200000 00000000 00000000
# DLL-NEXT: 180002040 08300080 01000000 04000000 0c000000
# DLL-NEXT: 180002050 00000000 00000000 00000000 78200000
# DLL-NEXT: 180002060 01000000 02000000 02000000 7e200000
# DLL-NEXT: 180002070 86200000 8e200000 642e646c 6c000830
# DLL-NEXT: 180002080 00004020 00009220 00009c20 00000000
# DLL-NEXT: 180002090 01005f5f 74705f75 6e697400 7824746c
# DLL:      Contents of section .sealed:
# DLL-NEXT: 180003000 00000000 00000000 f1f1f1f1 f1f1f1f1
# DLL-NEXT: 180003010 00000000 00000000 00000000 00000000

## The importer: x's offset at 0x3010, from the entry of x$tls at 0x2000,
## kind 5, and the record's word, 8; it holds 0xF1F1F1F1F1F1F1F1 until the
## fill writes it.
# USE:      Contents of section .rdata:
# USE-NEXT: 180002000 78200000 00000000 00000000 00000000
# USE-NEXT: 180002010 00000000 00000000 01000000 00000000
# USE-NEXT: 180002020 00300000 18000000 00000000 00000000
# USE-NEXT: 180002030 10300000 05200000 08000000 00000000
# USE:      Contents of section .sealed:
# USE-NEXT: 180003000 00000000 00000000 00000000 00000000
# USE-NEXT: 180003010 f1f1f1f1 f1f1f1f1 ........

## A DLL that reads its own x: the word at 0x3020 is computed from the pointer
## at 0x2050 to x's record at 0x2058, which names the place word at 0x3008.
# OWN:      Contents of section .rdata:
# OWN-NEXT: 180002000 f8200000 00000000 00000000 00000000
# OWN-NEXT: 180002010 00000000 00000000 02000000 00000000
# OWN-NEXT: 180002020 00300000 28000000 00000000 00000000
# OWN-NEXT: 180002030 08300000 02200000 00000000 00000000
# OWN-NEXT: 180002040 20300000 55200000 08000000 00000000
# OWN-NEXT: 180002050 58200080 01000000 08300080 01000000
# OWN-NEXT: 180002060 04000000 0c000000 00000000 00000000
# OWN:      Contents of section .sealed:
# OWN-NEXT: 180003000 00000000 00000000 f1f1f1f1 f1f1f1f1
# OWN-NEXT: 180003010 00000000 00000000 00000000 00000000
# OWN-NEXT: 180003020 f1f1f1f1 f1f1f1f1 ........

## The executable knows its place, 0x140, and writes x's offset, 0x144, into
## the code: a mov of an immediate, or a movz and a movk.
# LE-x64:        movq $0x144, %rax
# LE-x64-NEXT:   movl (%r14,%rax), %eax
# LE-arm64:      movz x8, #0x0, lsl #16
# LE-arm64-NEXT: movk x8, #0x144
# LE-arm64-NEXT: ldr w0, [x28, x8]

## An executable whose template holds a word that the fill writes registers
## again: its place, first in .sealed, holds 0x140 from the start, and the
## list names it, kind 2, before the word of the template.
# WORD:      Contents of section .rdata:
# WORD:      140002020 01000000 00000000 02000000 00000000
# WORD-NEXT: 140002030 00300000 20000000 00000000 00000000
# WORD-NEXT: 140002040 10400000 08000000 08300000 12200000
# WORD-NEXT: 140002050 00000000 00000000 10400000 00200000
# WORD:      Contents of section .sealed:
# WORD-NEXT: 140003000 00000000 00000000 40010000 00000000
# WORD-NEXT: 140003010 00000000 00000000 00000000 00000000

# WIDE:  error: GOTTPOFF relocation against x, a thread-local of w.dll, whose thread-locals are at no fixed offset from the thread pointer; reach it through its record x$tls
# NOREC: error: GOTTPOFF relocation against z, a thread-local of the image, whose place the link does not know and which has no record
# NAMED: error: undefined symbol: __declspec(dllimport) x$tpoff

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
        movq    x@gottpoff(%rip), %rax
        movl    (%r14,%rax), %eax
        retq

#--- use-aarch64.s
        .text
        .globl  use
use:
        adrp    x8, :gottprel:x
        ldr     x8, [x8, :gottprel_lo12:x]
        ldr     w0, [x28, x8]
        ret

#--- wide.def
LIBRARY w.dll
EXPORTS
x$tls DATA

#--- own-x86_64.s
        .section .tls$,"dw"
z:      .long   1
        .text
        .globl  own
own:
        movq    z@gottpoff(%rip), %rax
        retq

#--- own-aarch64.s
        .section .tls$,"dw"
z:      .long   1
        .text
        .globl  own
own:
        adrp    x8, :gottprel:z
        ldr     x8, [x8, :gottprel_lo12:z]
        ret

#--- named.s
        .data
        .quad   __imp_x$tpoff
