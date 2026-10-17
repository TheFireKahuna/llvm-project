# REQUIRES: x86
## Under -import-slots, an executable that does not replace an overridable
## default itself but links a DLL that offers one selects the offer for the
## program: it writes the table slot for the offered default to an import thunk
## that reaches the DLL's instance, and sets the anchor's mask bit, so the
## default is replaced for every image. The executable's own replacement, when
## it has one, wins the slot over an offer.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium offer.s -o offer.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records.s -o records.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium repl.s -o repl.obj
# RUN: lld-link -import-slots -dll -noentry records.obj -out:records.dll \
# RUN:   -implib:records.lib

## The DLL offers its allocator; its import library carries the offer symbol.
# RUN: lld-link -import-slots -dll -noentry offer.obj -out:offer.dll \
# RUN:   -implib:offer.lib
# RUN: llvm-nm offer.lib | FileCheck --check-prefix=IMPLIB %s
# IMPLIB: __imp___ovr_offer$_Znwy

## The executable defines no operator new, links the offering DLL and the
## runtime's import library, and selects the offer: slot 0 names the import thunk's
## RVA, and the anchor's mask marks slot 0 replaced.
# RUN: lld-link -import-slots main.obj offer.lib records.lib -out:main.exe \
# RUN:   -entry:main -subsystem:console
# RUN: od -A x -t x1 -j 0x40 -N 0x8 main.exe | FileCheck --check-prefix=SLOT %s
## Slot 0 holds the offered default's tag and the thunk's RVA (non-zero).
# SLOT: 000040 f3 a7 e1 c5 {{.. .. }}00 00

## The anchor's mask marks slot 0 replaced; the entry points into the image.
# RUN: llvm-objdump -s --section=.ovra main.exe | FileCheck --check-prefix=MASK %s
# MASK:      d5c23a7b 01000800 01000000 00000000

## The slot's RVA is an import thunk that reaches the DLL's instance, so the
## executable imports the offered default from the DLL under the offer name.
# RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=THUNK %s
# THUNK:      Name: offer.dll
# THUNK:      Symbol: __ovr_offer$_Znwy

## An executable that replaces the default itself wins the slot over the offer:
## slot 0 names its own body, not the DLL's thunk, and no offer import is used.
# RUN: lld-link -import-slots repl.obj main.obj offer.lib records.lib \
# RUN:   -out:repl.exe -entry:main -subsystem:console
# RUN: llvm-readobj --coff-imports repl.exe | FileCheck --check-prefix=LOCAL %s
# LOCAL-NOT: __ovr_offer$_Znwy

#--- offer.s
        .text
        .globl _Znwy
_Znwy:
        xorl %eax, %eax
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy /EXPORT:_Znwy"

#--- records.s
## Stands in for the C++ runtime, whose default records its slot.
        .text
        .weak _Znwy
_Znwy:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /EXPORT:_Znwy"

#--- main.s
        .text
        .globl main
main:
        retq
        .data
        .globl __ovr_check
__ovr_check:
        .quad 0

#--- repl.s
        .text
        .globl _Znwy
_Znwy:
        movl $1, %eax
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy /INCLUDE:__llvm_overrides_v1"
