# REQUIRES: x86
## Under -import-slots, an executable that replaces an overridable default
## carries one version-1 table in its header page, which the C++ runtime's
## defaults reach through the PEB at a fixed offset from the image base, and an
## anchor beside it in a read-only section of its own, the second copy of every
## slot that the runtime validates at load. The table holds, for each replaced
## slot, its tag and the replacement body's RVA; the anchor holds the magic,
## version, slot count and the mask of the replaced slots, then for each slot
## the body's address, relocated for the image, which also makes the body
## address-taken. The DOS program moves past the table and still runs.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium repl.s -o repl.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records.s -o records.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium noop.s -o noop.obj
# RUN: llvm-lib -out:records.lib records.obj
# RUN: lld-link -import-slots repl.obj records.lib -out:repl.exe \
# RUN:   -entry:_Znwy -subsystem:console

## The table at 0x40: slot 0 holds tag 0xC5E1A7F3 and the body's RVA 0x1000;
## the header at 0x80 holds the magic, version 1, slot count 8 and the anchor's
## RVA, then a zero extension RVA and reserved word.
# RUN: od -A x -t x1 -j 0x40 -N 0x50 repl.exe | FileCheck --check-prefix=TABLE %s
# TABLE: 000040 f3 a7 e1 c5 00 10 00 00 00 00 00 00 00 00 00 00
# TABLE: 000080 d5 c2 3a 7b 01 00 08 00 {{.. .. .. ..}} 00 00 00 00

## The anchor: magic, version 1, count 8, mask 0x1 (slot 0 replaced), reserved
## 0, then the body's address and seven null entries.
# RUN: llvm-objdump -s --section=.ovra repl.exe | FileCheck --check-prefix=ANCHOR %s
# ANCHOR:      d5c23a7b 01000800 01000000 00000000
# ANCHOR-NEXT: {{[0-9a-f]+}} 00100040 01000000 00000000 00000000

## The anchor's pointer to the body is relocated for the image.
# RUN: llvm-readobj --coff-basereloc repl.exe | FileCheck --check-prefix=RELOC %s
# RELOC: Type: DIR64

## The anchor is in a read-only section of its own.
# RUN: llvm-readobj --sections repl.exe | FileCheck --check-prefix=SECTION %s
# SECTION:      Name: .ovra
# SECTION:      Characteristics [
# SECTION-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# SECTION-NEXT:   IMAGE_SCN_MEM_READ
# SECTION-NEXT: ]

## The DOS program moves to a paragraph past the table, intact, so the stub
## still runs under DOS. Its first bytes open "This program cannot be run...".
# RUN: od -A x -t x1 -j 0xa0 -N 0x10 repl.exe | FileCheck --check-prefix=STUB %s
# STUB: 0000a0 0e 1f ba 0e 00 b4 09 cd 21 b8 01 4c cd 21 54 68

## An executable that replaces nothing writes no table and no anchor section.
# RUN: lld-link -import-slots records.obj noop.obj -out:noop.exe \
# RUN:   -entry:start -subsystem:console
# RUN: llvm-readobj --sections noop.exe | FileCheck --check-prefix=NONE %s
# NONE-NOT: Name: .ovra

#--- repl.s
        .text
        .globl _Znwy
_Znwy:
        retq
        .data
        .globl __ovr_check
__ovr_check:
        .quad 0
        .section .drectve,"yn"
        .ascii " /OVERRIDABLE:_Znwy /INCLUDE:__llvm_overrides_v1"

#--- records.s
        .globl __ovr_version
        .set __ovr_version, 1
        .globl "__ovr_slot$_Znwy"
        .set "__ovr_slot$_Znwy", 0

#--- noop.s
        .text
        .globl start
start:
        retq
