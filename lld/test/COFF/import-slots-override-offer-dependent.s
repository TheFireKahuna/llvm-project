# REQUIRES: x86
## Under -import-slots, an executable selects an overridable default's offer
## in ELF's breadth-first load order: its own replacement, then the offers of
## the DLLs whose import libraries it links, then those of the DLLs they depend
## on, which the vague-entity records of those import libraries describe. The
## executable imports such an offer from its DLL by name, as it would through
## the DLL's own import library, and writes the slot to that import's thunk.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium m.s -o m.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium n.s -o n.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records.s -o records.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium repl.s -o repl.obj
# RUN: lld-link -import-slots -dll -noentry records.obj -out:records.dll \
# RUN:   -implib:records.lib

## m.dll offers its allocator, and a.dll, which imports from m.dll, carries
## that offer in its import library's record.
# RUN: lld-link -import-slots -dll -noentry m.obj -out:m.dll -implib:m.lib
# RUN: lld-link -import-slots -dll -noentry n.obj -out:n.dll -implib:n.lib
# RUN: lld-link -import-slots -dll -noentry a.obj m.lib -out:a.dll -implib:a.lib

## The executable links only a.lib, and imports the offer from m.dll.
# RUN: lld-link -import-slots main.obj a.lib records.lib -out:main.exe \
# RUN:   -entry:main -subsystem:console -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=LOG %s
# RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=DEP %s
# RUN: od -A x -t x1 -j 0x40 -N 0x8 main.exe | FileCheck --check-prefix=SLOT %s
# LOG:      Importing __ovr_offer$_Znwy from m.dll, which a DLL the image imports depends on
# DEP:      Name: m.dll
# DEP-NEXT: ImportLookupTableRVA:
# DEP-NEXT: ImportAddressTableRVA:
# DEP-NEXT: Symbol: __ovr_offer$_Znwy
# SLOT:     000040 f3 a7 e1 c5 {{.. .. }}00 00

## An offer of a DLL whose import library the executable links comes first.
# RUN: lld-link -import-slots main.obj a.lib n.lib records.lib -out:direct.exe \
# RUN:   -entry:main -subsystem:console
# RUN: llvm-readobj --coff-imports direct.exe | FileCheck --check-prefix=DIRECT %s
# DIRECT:     Name: n.dll
# DIRECT-NOT: Name: m.dll

## The executable's own replacement comes before every offer.
# RUN: lld-link -import-slots repl.obj main.obj a.lib records.lib -out:repl.exe \
# RUN:   -entry:main -subsystem:console
# RUN: llvm-readobj --coff-imports repl.exe | FileCheck --check-prefix=LOCAL %s
# LOCAL-NOT: __ovr_offer$_Znwy

#--- m.s
        .text
        .globl _Znwy
_Znwy:
        xorl %eax, %eax
        retq
        .globl mfn
mfn:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy /EXPORT:_Znwy /EXPORT:mfn"

#--- n.s
        .text
        .globl _Znwy
_Znwy:
        movl $2, %eax
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy /EXPORT:_Znwy"

#--- a.s
        .text
        .globl afn
afn:
        jmp mfn
        .section .drectve,"yni"
        .ascii " /EXPORT:afn"

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
