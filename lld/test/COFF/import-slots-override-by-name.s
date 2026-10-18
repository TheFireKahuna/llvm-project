# REQUIRES: x86
## Under -import-slots, a DLL's exported weak definition that its object marks
## as an overridable default by name is listed in the DLL's import library. An
## executable that defines such a function, with or without a mark of its own,
## replaces it for every image through the override table's block: a fixed
## block at 0x7e0 in the header page, whose 64 buckets of {key, rva, info}
## follow at 0x800. A name's key is its xxh3_64 hash with bit 0 clear and its
## home bucket the hash's top six bits; an entry takes the first free bucket
## from there, in the order of the names. The table's header names the block,
## and the anchor holds the number of its entries and, in the block's order,
## each key with the address it names. The headers reach past the buckets.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium l.s -o l.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plain.s -o plain.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium offer.s -o offer.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc offer.s -o undesc.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry l.obj -out:l.dll -implib:l.lib

# RUN: lld-link -import-slots main.obj l.lib -out:main.exe -entry:main \
# RUN:   -subsystem:console
# RUN: od -A x -t x4 -v -j 0x80 -N 0x10 main.exe | FileCheck --check-prefix=HEADER %s
# RUN: od -A x -t x4 -v -j 0x7e0 -N 0x20 main.exe | FileCheck --check-prefix=BLOCK %s
# RUN: od -A x -t x4 -v -j 0xae0 -N 0x30 main.exe | FileCheck --check-prefix=BUCKETS %s
# RUN: llvm-readobj --file-headers main.exe | FileCheck --check-prefix=SIZE %s
# RUN: llvm-objdump -s --section=.ovra main.exe | FileCheck --check-prefix=ANCHOR %s

## The header: the magic, version 1 and eight slots, the anchor's RVA, and the
## block's RVA.
# HEADER: 000080 7b3ac2d5 00080001 {{[0-9a-f]+}} 000007e0
## The block: its magic, version 1 and three entries, and no overflow list.
# BLOCK:      0007e0 2f1b8e57 00030001 00000000 00000000
# BLOCK-NEXT: 0007f0 00000000 00000000 00000000 00000000
## hook takes its home bucket 46, hook133 its home bucket 47, and hook156,
## whose home bucket is 46 too, the next free one, 48.
# BUCKETS:      000ae0 5a5e31d2 ba8e35e1 00001006 00000000
# BUCKETS-NEXT: 000af0 f426618e bfc31fe3 0000100c 00000000
# BUCKETS-NEXT: 000b00 5fc6c1dc bb8850b6 00001012 00000000
# SIZE: SizeOfHeaders: 3072
## The anchor holds three entries after its eight slots, each key with the
## address it names.
# ANCHOR:      d5c23a7b 01000800 00000000 03000000
# ANCHOR:      d2315e5a e1358eba 06100040 01000000
# ANCHOR-NEXT: 8e6126f4 e31fc3bf 0c100040 01000000
# ANCHOR-NEXT: dcc1c65f b65088bb 12100040 01000000

## Each body the block names is a target of the dispatch from its default, so
## the guard function table lists it.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc config.s -o config.obj
# RUN: lld-link -import-slots -guard:cf main.obj config.obj l.lib -out:guard.exe \
# RUN:   -entry:main -subsystem:console
# RUN: llvm-readobj --coff-load-config guard.exe | FileCheck --check-prefix=GUARD %s
# GUARD:      GuardFidTable [
# GUARD-NEXT:   0x140001006
# GUARD-NEXT:   0x14000100C
# GUARD-NEXT:   0x140001012
# GUARD-NEXT: ]

## An executable that replaces nothing writes no table.
# RUN: lld-link -import-slots plain.obj l.lib -out:plain.exe -entry:main \
# RUN:   -subsystem:console
# RUN: llvm-readobj --sections plain.exe | FileCheck --check-prefix=PLAIN %s
# PLAIN-NOT: .ovra

## A DLL that reaches such a default has cells of it, which its fill of cells
## bound by name writes with the address the anchor's entry with the default's
## key names: a list holds each cell under its key, the import's entry marked
## read-only, and the fill writes that one between two protection calls, made
## only once a cell is to be written. With nothing shared it does no search.
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj l.lib ntdll.lib \
# RUN:   -out:use.dll
# RUN: llvm-readobj --coff-imports use.dll > use.txt
# RUN: llvm-objdump -s -j .rdata use.dll >> use.txt
# RUN: llvm-objdump -d --print-imm-hex use.dll >> use.txt
# RUN: FileCheck --check-prefix=FILL --input-file=use.txt %s
# FILL:      Name: l.dll
# FILL-NEXT: ImportLookupTableRVA:
# FILL-NEXT: ImportAddressTableRVA:
# FILL-NEXT: Symbol: hook
# FILL:      Name: ntdll.dll
# FILL-NEXT: ImportLookupTableRVA:
# FILL-NEXT: ImportAddressTableRVA: 0x[[#%X,PROTECT:]]
# FILL-NEXT: Symbol: NtProtectVirtualMemory
# FILL-NEXT: }
# FILL:      Name: l.dll
# FILL-NEXT: ImportLookupTableRVA:
# FILL-NEXT: ImportAddressTableRVA: 0x3000
# FILL-NEXT: Symbol: hook
## Each entry is {key, rva, kind}, the key's bit 0 marking a read-only cell;
## a default bound by name is of kind 0.
# FILL:      180002000 d2315e5a e1358eba 00300000 00000000
# FILL-NEXT: 180002010 d3315e5a e1358eba {{[0-9a-f]+}} 00000000
# FILL:      cmpl $0x7b3ac2d5, 0x80(%rdi)
# FILL:      leaq {{.*}}(%rip), %rbx # 0x180002000
# FILL:      cmpq %r12, (%rax)
# FILL-NEXT: je
# FILL:      testb $0x1, (%rbx)
# FILL:      movl $0x4, %r9d
# FILL:      callq *{{.*}}(%rip) # 0x[[#%x,0x180000000+PROTECT]]
# FILL:      movq %r13, (%rcx,%rax)
# FILL:      movq 0x8(%rax), %r13
# FILL-NEXT: jmp
# RUN: FileCheck --check-prefix=NOSEARCH --input-file=use.txt %s
# NOSEARCH-NOT: LdrAddRefDll
# NOSEARCH-NOT: 0x6c8e9cf5

## A DLL whose strong definition of such a function the image exports offers
## it, marked or not, and reaches it as every other image does: its described
## address-take loads the default's import entry, which the fill writes.
## Without the library's import library the address stays direct.
# RUN: lld-link -import-slots -dll -noentry offer.obj crt.obj l.lib ntdll.lib \
# RUN:   -out:offer.dll
# RUN: llvm-readobj --coff-exports --coff-imports offer.dll > offer.txt
# RUN: llvm-objdump -d offer.dll >> offer.txt
# RUN: FileCheck --check-prefix=OFFER --input-file=offer.txt %s
# OFFER:      Name: l.dll
# OFFER-NEXT: ImportLookupTableRVA:
# OFFER-NEXT: ImportAddressTableRVA: 0x[[#%X,OENTRY:]]
# OFFER-NEXT: Symbol: hook
# OFFER:      Name: __ovr_offer$hook
# OFFER:      <take>:
# OFFER-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x[[#%x,0x180000000+OENTRY]]
# RUN: lld-link -import-slots -dll -noentry offer.obj crt.obj -out:own.dll
# RUN: llvm-objdump -d own.dll | FileCheck --check-prefix=OWN %s
# OWN:      <take>:
# OWN-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax

## An object that does not describe its references cannot have them reach the
## program's choice.
# RUN: not lld-link -import-slots -dll -noentry undesc.obj crt.obj l.lib \
# RUN:   ntdll.lib -out:undesc.dll 2>&1 | FileCheck --check-prefix=UNDESC %s
# UNDESC: error: undesc.obj: hook is offered as an overridable default, but its object does not describe its references to it, which therefore cannot reach the program's choice

#--- l.s
        .text
        .weak hook
hook:
        xorl %eax, %eax
        retq
        .weak hook133
hook133:
        xorl %eax, %eax
        retq
        .weak hook156
hook156:
        xorl %eax, %eax
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLEBYNAME:hook /EXPORT:hook"
        .ascii " /OVERRIDABLEBYNAME:hook133 /EXPORT:hook133"
        .ascii " /OVERRIDABLEBYNAME:hook156 /EXPORT:hook156"

#--- main.s
## Compiled for Control Flow Guard, so no symbol is taken as address-taken
## by default.
        .def @feat.00; .scl 3; .type 0; .endef
        .globl @feat.00
@feat.00 = 0x800
        .text
        .globl main
main:
        callq *__imp_hook(%rip)
        retq
        .def hook; .scl 2; .type 32; .endef
        .globl hook
hook:
        movl $1, %eax
        retq
        .def hook133; .scl 2; .type 32; .endef
        .globl hook133
hook133:
        movl $2, %eax
        retq
        .def hook156; .scl 2; .type 32; .endef
        .globl hook156
hook156:
        movl $3, %eax
        retq
        .data
        .globl __ovr_check
__ovr_check:
        .quad 0

#--- plain.s
        .text
        .globl main
main:
        callq *__imp_hook(%rip)
        retq

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
        .section .CRT$XIA,"dr"
        .globl __xi_a
__xi_a:
        .quad 0
        .section .CRT$XIZ,"dr"
        .globl __xi_z
__xi_z:
        .quad 0

#--- offer.s
        .text
        .globl take
take:
        leaq hook(%rip), %rax
        retq
        .def hook; .scl 2; .type 32; .endef
        .globl hook
hook:
        movl $4, %eax
        retq
        .section .drectve,"yni"
        .ascii " /EXPORT:hook /EXPORT:take"

#--- use.s
        .text
        .globl dllmain
dllmain:
        callq *__imp_hook(%rip)
        retq
        .data
        .globl ghook
ghook:
        .quad hook
        .section .drectve,"yni"
        .ascii " /INCLUDE:ghook"

#--- config.s
  .section .rdata,"dr"
  .p2align 3
  .globl _load_config_used
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 12, 1, 0
  .quad __guard_iat_table
  .quad __guard_iat_count
  .quad __guard_longjmp_table
  .quad __guard_fids_count
  .fill 84, 1, 0
