# REQUIRES: x86
## Under -import-slots, a word of an image's static data that holds the address
## of an overridable default it imports is a cell: an in-place import slot the
## loader fills with the runtime's entry. The image's fill, a TLS callback that
## runs before any of its code, reads the executable's override table through
## the PEB and, for a slot the table names replaced, overwrites the cell with
## the executable's image base plus the slot's RVA, so every image observes the
## default's address as the one replacement. A DLL loads the runtime's records
## member for its imports' slots, as the executable does for its replacements.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records.s -o records.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: llvm-lib -out:records.lib records.obj
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib

# RUN: lld-link -import-slots -dll -noentry use.obj rt.lib records.lib \
# RUN:   -out:use.dll

## The cell holds the address of the imported default, so the DLL imports it;
## the word is an in-place import slot the loader fills, which the fill below
## then overwrites.
# RUN: llvm-readobj --coff-imports use.dll | FileCheck --check-prefix=IMPORT %s
# IMPORT:      Name: rt.dll
# IMPORT:      Symbol: _Znwy

## The fill is a TLS callback: for process attach only, since a later thread
## attach must not undo the program's own assignment to the cell, it reads the
## executable's image base through the PEB, checks the table's magic, and for
## slot 0's tag writes base + the slot's RVA into the cell.
# RUN: llvm-objdump -d use.dll | FileCheck --check-prefix=FILL %s
# FILL:      cmpl    $0x1, %edx
# FILL-NEXT: jne
# FILL-NEXT: movq    %gs:0x60, %r11
# FILL-NEXT: movq    0x10(%r11), %r11
# FILL-NEXT: cmpl    $0x7b3ac2d5, 0x80(%r11)
# FILL-NEXT: jne
# FILL-NEXT: cmpl    $0xc5e1a7f3, 0x40(%r11)
# FILL-NEXT: jne
# FILL-NEXT: movl    0x44(%r11), %eax
# FILL-NEXT: addq    %r11, %rax
# FILL-NEXT: movq    %rax,
# FILL-NEXT: retq

## The fill's pointer is in the TLS callback array, which the loader runs, right
## after the executable's validation and before the program's own callbacks; the
## runtime's TLS directory brackets it, so the loader reaches it.
# RUN: llvm-readobj --sections use.dll | FileCheck --check-prefix=CRT %s
# CRT: Name: .CRT

## A DLL that imports no overridable default, or takes no address, carries no
## fill: the cell is the whole mechanism.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plain.s -o plain.obj
# RUN: lld-link -import-slots -dll -noentry plain.obj rt.lib records.lib \
# RUN:   -out:plain.dll
# RUN: llvm-objdump -d plain.dll | FileCheck --check-prefix=NONE %s
# NONE-NOT: %gs:0x60

## Without a TLS directory the loader never calls the fill, so a cell would keep
## the runtime's entry: that is an error, not a silently unreplaced default.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium notls.s -o notls.obj
# RUN: not lld-link -import-slots -dll -noentry notls.obj rt.lib records.lib \
# RUN:   -out:notls.dll 2>&1 | FileCheck --check-prefix=NOTLS %s
# NOTLS: error: notls.obj: the address of an overridable default needs a TLS directory to fill, but _tls_used is not defined

#--- rt.s
        .text
        .globl _Znwy
_Znwy:
        retq
        .section .drectve,"yni"
        .ascii " /EXPORT:_Znwy"

#--- records.s
        .globl __ovr_version
        .set __ovr_version, 1
        .globl "__ovr_slot$_Znwy"
        .set "__ovr_slot$_Znwy", 0

#--- use.s
        .text
        .globl dllmain
dllmain:
        callq _Znwy
        retq
        .data
        .globl gnew
gnew:
        .quad _Znwy
## A minimal TLS directory with its callback-array bounds, as the C++ runtime
## provides, so the linker's callback joins the array the loader runs.
        .section .CRT$XLA,"dr"
        .globl __xl_a
__xl_a:
        .quad 0
        .section .CRT$XLZ,"dr"
        .globl __xl_z
__xl_z:
        .quad 0
        .section .rdata,"dr"
        .globl _tls_used
        .p2align 3
_tls_used:
        .quad 0
        .quad 0
        .quad 0
        .quad __xl_a + 8
        .long 0
        .long 0

#--- plain.s
        .text
        .globl dllmain
dllmain:
        callq _Znwy
        retq
        .data
        .globl n
n:
        .long 0

#--- notls.s
        .text
        .globl dllmain
dllmain:
        retq
        .data
        .globl gnew
gnew:
        .quad _Znwy
