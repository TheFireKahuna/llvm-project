# REQUIRES: x86
## Under -import-slots, the C++ runtime's definition of an overridable default
## records its slot, and the slot of the default it forwards to, in its
## /overridable: directive, and the runtime's link writes them into its import
## library, from which an executable that replaces a default reads the slot.
## The linker rejects a record of a table format it does not write, a
## replacement no record gives a slot for, two inputs that give one default
## different slots, and a malformed directive. A replacing object references
## __llvm_overrides_v1, which the linker defines only under -import-slots, so a
## link without it, such as one by another linker, fails loudly rather than
## leaving a split heap. A program that defines a replaceable function with no
## such runtime keeps its own definition and needs no table.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium repl.s -o repl.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium repl-delete.s -o repl-delete.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium other.s -o other.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium bad.s -o bad.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium v2.s -o v2.obj
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib
# RUN: llvm-lib -out:v2.lib v2.obj

## The slot comes from the runtime's import library: the table names the
## replacement in slot 0, and forwards slot 1, whose default calls slot 0's.
# RUN: lld-link -import-slots repl.obj rt.lib -out:ok.exe -entry:main \
# RUN:   -subsystem:console
# RUN: od -A x -t x4 -v -j 0x40 -N 0x10 ok.exe | FileCheck --check-prefix=TABLE %s
# TABLE: 000040 c5e1a7f3 {{0*[1-9a-f][0-9a-f]*}} c5e1a7f2 [[RVA:[0-9a-f]+]]

## Without -import-slots the marker is undefined, so the link fails loudly.
# RUN: not lld-link repl.obj rt.lib -out:plain.exe -entry:main \
# RUN:   -subsystem:console 2>&1 | FileCheck --check-prefix=MARKER %s
# MARKER: undefined symbol: __llvm_overrides_v1

## A record of a table format the linker does not write is rejected.
# RUN: not lld-link -import-slots repl.obj v2.lib -out:v2.exe -entry:main \
# RUN:   -subsystem:console 2>&1 | FileCheck --check-prefix=VERSION %s
# VERSION: override table format (version 2) is not the one this linker writes (version 1)

## A replacement that no record gives a slot for is rejected.
# RUN: not lld-link -import-slots repl-delete.obj rt.lib -out:ns.exe \
# RUN:   -entry:main -subsystem:console 2>&1 | FileCheck --check-prefix=SLOT %s
# SLOT: operator delete(void*) replaces an overridable default, but the C++ runtime records no slot for it

## Two inputs that give one default different slots are rejected.
# RUN: not lld-link -import-slots -dll -noentry rt.obj other.obj -out:two.dll \
# RUN:   2>&1 | FileCheck --check-prefix=CONFLICT %s
# CONFLICT: other.obj records overridable default _Znwy with slot 3, but another input with slot 0 or another forwarding

## A malformed directive is rejected.
# RUN: not lld-link -import-slots -dll -noentry bad.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD %s
# BAD: bad.obj: malformed /overridable: directive: _Znwy,x

## With no runtime to override, the replacement needs no table, and the link
## holds with nothing read.
# RUN: lld-link -import-slots repl.obj -out:free.exe -entry:main \
# RUN:   -subsystem:console

#--- rt.s
        .text
        .weak _Znwy
_Znwy:
        retq
        .weak _Znay
_Znay:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /OVERRIDABLE:_Znay,1,0"
        .ascii " /EXPORT:_Znwy /EXPORT:_Znay"

#--- repl.s
        .text
        .globl main
main:
        retq
        .globl _Znwy
_Znwy:
        retq
        .data
## Stands in for the C++ runtime's start-up validation, which a replacing link
## names.
        .globl __ovr_check
__ovr_check:
        .quad 0
        .section .drectve,"yn"
        .ascii " /OVERRIDABLE:_Znwy /INCLUDE:__llvm_overrides_v1"

#--- repl-delete.s
        .text
        .globl main
main:
        retq
        .globl _ZdlPv
_ZdlPv:
        retq
        .data
        .globl __ovr_check
__ovr_check:
        .quad 0
        .section .drectve,"yn"
        .ascii " /OVERRIDABLE:_ZdlPv /INCLUDE:__llvm_overrides_v1"

#--- other.s
        .section .drectve,"yn"
        .ascii " /OVERRIDABLE:_Znwy,3"

#--- bad.s
        .section .drectve,"yn"
        .ascii " /OVERRIDABLE:_Znwy,x"

#--- v2.s
## The record member of a runtime whose table format is version 2: the
## LinkRecordOverrideSlots group (24) holds the version, then _Znwy in slot 0
## with no forwarding.
        .globl "__llvm_vague$rt2.dll"
"__llvm_vague$rt2.dll":
        .section .llvm_link_records,"yn"
        .ascii "LLRC"
        .byte 1, 0
        .byte 24, 9
        .byte 2
        .byte 5
        .ascii "_Znwy"
        .byte 0, 0, 0
