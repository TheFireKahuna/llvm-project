# REQUIRES: x86
## Under -import-slots, an executable that defines a strong replacement of an
## overridable default reads the default's slot from the C++ runtime's import
## library, a regular member that defines each slot as an absolute symbol and
## is loaded only when a link replaces a default. The linker rejects a records
## member whose format version it does not write, and a replacement the member
## records no slot for. A replacing object references __llvm_overrides_v1, which
## the linker defines only under -import-slots, so a link without it, such as
## one by another linker, fails loudly rather than leaving a split heap. A
## program that defines a replaceable function with no such runtime keeps its
## own definition and needs no table.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium repl.s -o repl.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records.s -o records.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records-v2.s -o records-v2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records-noslot.s -o records-noslot.obj
# RUN: llvm-lib -out:records.lib records.obj
# RUN: llvm-lib -out:records-v2.lib records-v2.obj
# RUN: llvm-lib -out:records-noslot.lib records-noslot.obj

## The records member is loaded and its version and slot read: the link holds.
# RUN: lld-link -import-slots repl.obj records.lib -out:ok.exe \
# RUN:   -entry:_Znwy -subsystem:console

## Without -import-slots the marker is undefined, so the link fails loudly.
# RUN: not lld-link repl.obj records.lib -out:plain.exe -entry:_Znwy \
# RUN:   -subsystem:console 2>&1 | FileCheck --check-prefix=MARKER %s
# MARKER: undefined symbol: __llvm_overrides_v1

## A records member of a format version the linker does not write is rejected.
# RUN: not lld-link -import-slots repl.obj records-v2.lib -out:v2.exe \
# RUN:   -entry:_Znwy -subsystem:console 2>&1 | FileCheck --check-prefix=VERSION %s
# VERSION: override table format (version 2) is not the one this linker writes (version 1)

## A replacement the records member records no slot for is rejected.
# RUN: not lld-link -import-slots repl.obj records-noslot.lib -out:ns.exe \
# RUN:   -entry:_Znwy -subsystem:console 2>&1 | FileCheck --check-prefix=SLOT %s
# SLOT: operator new(unsigned long long) replaces an overridable default, but the C++ runtime's import library records no slot for it

## With no runtime to override, the replacement needs no table, and the link
## holds with nothing read.
# RUN: lld-link -import-slots repl.obj -out:free.exe -entry:_Znwy \
# RUN:   -subsystem:console

#--- repl.s
        .text
        .globl _Znwy
_Znwy:
        retq
        .data
## Stands in for the C++ runtime's start-up validation, which a replacing link
## names so its member, and the table it reads, come in together.
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

#--- records-v2.s
        .globl __ovr_version
        .set __ovr_version, 2
        .globl "__ovr_slot$_Znwy"
        .set "__ovr_slot$_Znwy", 0

#--- records-noslot.s
        .globl __ovr_version
        .set __ovr_version, 1
