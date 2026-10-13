# REQUIRES: x86
## Under -import-slots, a DLL's cells of an overridable default it imports are
## the words through which it reaches the default: each in-place import slot of
## it, and its import's entry, through which the DLL calls it and loads its
## address. The loader writes them with the runtime's entry. The DLL's fill,
## which its C initializer table runs before every other initializer, reads the
## executable's override table through the PEB and, for a slot the anchor's
## mask names replaced, writes the executable's image base plus the slot's RVA
## into each of its cells, so every image observes and calls the one
## replacement. A forwarded or unreplaced slot, or an executable with no table,
## leaves every cell the loader's value. A DLL reads its imports' slots from
## the runtime's import library, which records them as its defaults do.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib

## The DLL calls the default, and holds its address in writable and in
## read-only data. The read-only word is the slot its import is read from, so
## the call's thunk reads it too.
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:use.dll
# RUN: llvm-readobj --coff-imports use.dll > use.txt
# RUN: llvm-objdump -d --print-imm-hex use.dll >> use.txt
# RUN: FileCheck --ignore-case --check-prefixes=IMPORT,FILL --input-file=use.txt \
# RUN:   %s
# IMPORT:      Name: rt.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA: 0x[[RO:[0-9A-F]+]]
# IMPORT-NEXT: Symbol: _Znwy
# IMPORT:      Name: rt.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA: 0x[[RW:[0-9A-F]+]]
# IMPORT-NEXT: Symbol: _Znwy
# IMPORT:      Name: ntdll.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA: 0x[[PROTECT:[0-9A-F]+]]
# IMPORT-NEXT: Symbol: NtProtectVirtualMemory

## The fill reads the anchor's mask and writes the writable cell for slot 0 in
## place. Only when a read-only cell's slot is replaced does it make their range
## writable, write them, and restore the protection the first call reported. It
## returns 0, or the status of a failed protection call, which fails start-up.
# FILL:      subq    $0x48, %rsp
# FILL-NEXT: movq    %gs:0x60, %r11
# FILL-NEXT: movq    0x10(%r11), %r11
# FILL-NEXT: cmpl    $0x7b3ac2d5, 0x80(%r11)
# FILL-NEXT: jne     0x[[ZERO:[0-9a-f]+]]
# FILL-NEXT: movl    0x88(%r11), %r10d
# FILL-NEXT: movl    0x8(%r11,%r10), %r10d
# FILL-NEXT: btl     $0x0, %r10d
# FILL-NEXT: jae
# FILL-NEXT: movl    0x44(%r11), %eax
# FILL-NEXT: addq    %r11, %rax
# FILL-NEXT: movq    %rax, {{.*}}(%rip) # 0x18000[[RW]]
# FILL-NEXT: testl   $0x1, %r10d
# FILL-NEXT: je      0x[[ZERO]]
# FILL-NEXT: leaq    {{.*}}(%rip), %rax # 0x18000[[RO]]
# FILL-NEXT: movq    %rax, 0x30(%rsp)
# FILL-NEXT: movq    $0x8, 0x38(%rsp)
# FILL-NEXT: movl    $0x4, %r9d
# FILL-NEXT: leaq    0x40(%rsp), %rax
# FILL-NEXT: movq    %rax, 0x20(%rsp)
# FILL-NEXT: movq    $-0x1, %rcx
# FILL-NEXT: leaq    0x30(%rsp), %rdx
# FILL-NEXT: leaq    0x38(%rsp), %r8
# FILL-NEXT: callq   *{{.*}}(%rip) # 0x18000[[PROTECT]]
# FILL-NEXT: testl   %eax, %eax
# FILL-NEXT: jne     0x[[DONE:[0-9a-f]+]]
# FILL-NEXT: movq    %gs:0x60, %r11
# FILL-NEXT: movq    0x10(%r11), %r11
# FILL-NEXT: movl    0x88(%r11), %r10d
# FILL-NEXT: movl    0x8(%r11,%r10), %r10d
# FILL-NEXT: btl     $0x0, %r10d
# FILL-NEXT: jae
# FILL-NEXT: movl    0x44(%r11), %eax
# FILL-NEXT: addq    %r11, %rax
# FILL-NEXT: movq    %rax, {{.*}}(%rip) # 0x18000[[RO]]
# FILL-NEXT: movl    0x40(%rsp), %r9d
# FILL-NEXT: leaq    0x40(%rsp), %rax
# FILL-NEXT: movq    %rax, 0x20(%rsp)
# FILL-NEXT: movq    $-0x1, %rcx
# FILL-NEXT: leaq    0x30(%rsp), %rdx
# FILL-NEXT: leaq    0x38(%rsp), %r8
# FILL-NEXT: callq   *{{.*}}(%rip) # 0x18000[[PROTECT]]
# FILL-NEXT: jmp     0x[[DONE]]
# FILL-NEXT: [[ZERO]]: {{.*}} xorl %eax, %eax
# FILL-NEXT: [[DONE]]: {{.*}} addq $0x48, %rsp
# FILL-NEXT: retq
# FILL:      jmpq    *{{.*}}(%rip) # 0x18000[[RO]]

## The protection calls give the fill a frame, which its unwind information
## describes, and the C initializer table holds its pointer right after the
## table's start.
# RUN: llvm-readobj --unwind use.dll | FileCheck --check-prefix=UNWIND %s
# UNWIND:      StartAddress: (0x180001010)
# UNWIND:      PrologSize: 4
# UNWIND:      0x04: ALLOC_SMALL size=72
# RUN: llvm-objdump -s -j .CRT use.dll | FileCheck --check-prefix=CRT %s
# CRT:      Contents of section .CRT:
# CRT-NEXT: 00000000 00000000 10100080 01000000

## With only a writable cell, which is then the slot its import is read from,
## the fill is a leaf that needs no protection call, and so no ntdll.lib.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rw.s -o rw.obj
# RUN: lld-link -import-slots -dll -noentry rw.obj crt.obj rt.lib \
# RUN:   -out:rw.dll
# RUN: llvm-objdump -d --print-imm-hex rw.dll | FileCheck --check-prefix=LEAF %s
# LEAF-NOT:  subq
# LEAF:      btl     $0x0, %r10d
# LEAF-NOT:  callq
# LEAF:      xorl    %eax, %eax
# LEAF-NEXT: retq

## A DLL that reaches no overridable default carries no fill.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plain.s -o plain.obj
# RUN: lld-link -import-slots -dll -noentry plain.obj crt.obj rt.lib \
# RUN:   -out:plain.dll
# RUN: llvm-objdump -d plain.dll | FileCheck --check-prefix=NONE %s
# NONE-NOT: %gs:0x60

## Without a C initializer table nothing runs the fill, and without
## NtProtectVirtualMemory it cannot write a read-only cell: either is an error,
## not a silently unreplaced default.
# RUN: not lld-link -import-slots -dll -noentry use.obj rt.lib \
# RUN:   ntdll.lib -out:nocrt.dll 2>&1 | FileCheck --check-prefix=NOCRT %s
# NOCRT: error: the image reaches __declspec(dllimport) operator new(unsigned long long), an overridable default imported from rt.dll, through a cell that the image has no C initializer table (__xi_a) to fill
# RUN: not lld-link -import-slots -dll -noentry use.obj crt.obj rt.lib \
# RUN:   -out:noprotect.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=NOPROTECT %s
# NOPROTECT: error: the image reaches __declspec(dllimport) operator new(unsigned long long), an overridable default imported from rt.dll, through a cell that it cannot write without NtProtectVirtualMemory from ntdll.lib

## A thread-local cell is a slot in the template of the DLL's thread-local
## data, which the loader binds before it copies the template into any thread's
## block. A replaced one is written in the template, which later threads copy,
## and in the loading thread's block, found through the index and the template
## start that the TLS directory names.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -dll -noentry tl.obj crt.obj rt.lib \
# RUN:   -out:tl.dll
# RUN: llvm-readobj --coff-imports --sections tl.dll > tl.txt
# RUN: llvm-objdump -d --print-imm-hex tl.dll >> tl.txt
# RUN: FileCheck --ignore-case --check-prefix=TL --input-file=tl.txt %s
# TL:      Name: .data
# TL-NEXT: VirtualSize:
# TL-NEXT: VirtualAddress: 0x[[DATA:[0-9A-F]+]]
# TL:      Name: .tls
# TL-NEXT: VirtualSize:
# TL-NEXT: VirtualAddress: 0x[[TLS:[0-9A-F]+]]
# TL:      Name: rt.dll
# TL-NEXT: ImportLookupTableRVA:
# TL-NEXT: ImportAddressTableRVA: 0x[[CELL:[0-9A-F]+]]
# TL-NEXT: Symbol: _Znwy
# TL:      btl     $0x0, %r10d
# TL-NEXT: jae
# TL-NEXT: movl    0x44(%r11), %eax
# TL-NEXT: addq    %r11, %rax
# TL-NEXT: movq    %rax, {{.*}}(%rip) # 0x18000[[CELL]]
# TL-NEXT: movl    {{.*}}(%rip), %ecx # 0x18000[[DATA]]
# TL-NEXT: movq    %gs:0x58, %rdx
# TL-NEXT: movq    (%rdx,%rcx,8), %rdx
# TL-NEXT: movq    %rax, 0x8(%rdx)
# TL-NEXT: xorl    %eax, %eax
# TL-NEXT: retq

## A TLS directory that names no template or index leaves no thread's block
## to write.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tlbad.s -o tlbad.obj
# RUN: not lld-link -import-slots -dll -noentry tlbad.obj crt.obj rt.lib \
# RUN:   -out:tlbad.dll 2>&1 | FileCheck --check-prefix=TLBAD %s
# TLBAD: error: the image reaches __declspec(dllimport) operator new(unsigned long long), an overridable default imported from rt.dll, through a cell in thread-local data, but the image's TLS directory (_tls_used) names no template or index

## An executable binds its own references at link time and carries no fill.
# RUN: lld-link -import-slots -entry:dllmain -subsystem:console use.obj crt.obj \
# RUN:   rt.lib ntdll.lib -out:use.exe
# RUN: llvm-objdump -d use.exe | FileCheck --check-prefix=NONE %s

#--- rt.s
        .text
        .weak _Znwy
_Znwy:
        retq
        .globl other
other:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /EXPORT:_Znwy /EXPORT:other"

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
## The C initializer table's bounds, as the C runtime provides them, so the
## linker's fill joins the table the runtime's start-up runs.
        .section .CRT$XIA,"dr"
        .globl __xi_a
__xi_a:
        .quad 0
        .section .CRT$XIZ,"dr"
        .globl __xi_z
__xi_z:
        .quad 0

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
        .section .rdata,"dr",discard,cnew
        .globl cnew
        .p2align 3
cnew:
        .quad _Znwy

#--- rw.s
        .text
        .globl dllmain
dllmain:
        retq
        .data
        .globl gnew
gnew:
        .quad _Znwy

#--- plain.s
        .text
        .globl dllmain
dllmain:
        callq other
        retq
        .data
        .globl gother
gother:
        .quad other

#--- tl.s
        .text
        .globl dllmain
dllmain:
        retq
## The template starts with another thread-local datum, so the cell is at
## offset 8 in each thread's block.
        .section .tls,"dw"
        .globl _tls_start
_tls_start:
        .quad 0
        .section .tls$,"dw"
        .globl tnew
        .p2align 3
tnew:
        .quad _Znwy
        .section .tls$ZZZ,"dw"
        .globl _tls_end
_tls_end:
        .quad 0
        .data
        .globl _tls_index
_tls_index:
        .long 0
        .section .rdata,"dr"
        .globl _tls_used
        .p2align 3
_tls_used:
        .quad _tls_start
        .quad _tls_end
        .quad _tls_index
        .quad 0
        .long 0
        .long 0

#--- tlbad.s
        .text
        .globl dllmain
dllmain:
        retq
        .section .tls$,"dw"
        .globl tnew
        .p2align 3
tnew:
        .quad _Znwy
        .section .rdata,"dr"
        .globl _tls_used
        .p2align 3
_tls_used:
        .quad 0
        .quad 0
        .quad 0
        .quad 0
        .long 0
        .long 0
