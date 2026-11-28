# REQUIRES: x86, aarch64

## The link of an executable that defines a function which an overridable
## default has a slot for writes the override table into the executable's
## header page, after the DOS header: each slot as its tag and the RVA of what
## stands for its default, the table's header after the eighth slot, and the
## DOS program, which the DOS header's size leads to, after the last slot. A
## slot that the executable replaces names the replacement, which is then
## what every reference of the executable to its name binds, through the
## name's import pointer too, and what it exports as the name; it imports
## nothing of the name. One of hidden visibility is the executable's alone and
## replaces nothing, and one that no default has a slot for is an error where
## the executable shares a runtime with a DLL. One whose
## default forwards, by one step or several, to a default that the executable
## replaces names that replacement where no argument is dropped on the way,
## and otherwise a thunk with the first default's type prefix, which the link
## writes from the default's type ID and the executable's prefix key, then
## jumps to the replacement, after moving the third argument down where the
## second of more is dropped. Defaults of one type ID, replacement and drop
## share a thunk. The anchor, alone in the section .ovra, repeats the header
## and has the mask of the replaced slots, each slot's address, which makes
## each a target of Control Flow Guard, and then, for each slot, the slot
## whose replacement a call of its default reaches: that of a default which
## forwards across a dropped last argument, since a call checks no type. So
## do the executable's own calls of a default that it does not replace, and
## its address-takes of one read its import. A link that replaces nothing
## writes no table, and one with a table takes no DOS stub.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{e} = itanium
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-%{e}
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console \
# DEFINE:   -debug:symtab -opt:ref -guard:cf -out:a.exe
# DEFINE: %{run} = \
# DEFINE:   %{mc} rt.s -o rt.obj && %{mc} --defsym=%{a}=1 main.s -o main.obj && \
# DEFINE:   %{mc} --defsym=%{a}=1 --defsym=NONE=1 main.s -o none.obj && \
# DEFINE:   %{mc} --defsym=%{a}=1 --defsym=KEY=1 main.s -o key.obj && \
# DEFINE:   %{mc} --defsym=%{a}=1 --defsym=HIDDEN=1 main.s -o hidden.obj && \
# DEFINE:   %{mc} unnamed.s -o unnamed.obj && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry rt.obj -out:rt.dll \
# DEFINE:     -implib:rt.lib && \
# DEFINE:   %{link} main.obj rt.lib && llvm-nm a.exe > a.txt && \
# DEFINE:   od -v -A x -t x4 -N 0x100 a.exe >> a.txt && \
# DEFINE:   llvm-objdump -s -j .ovra a.exe >> a.txt && \
# DEFINE:   FileCheck --input-file=a.txt --check-prefix=TABLE %s && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn a.exe \
# DEFINE:     | FileCheck --check-prefix=%{a}-%{e} %s && \
# DEFINE:   llvm-objdump -s -j .text a.exe | FileCheck --check-prefix=TEXT %s && \
# DEFINE:   llvm-readobj --sections --coff-basereloc --coff-load-config \
# DEFINE:     --coff-imports --coff-exports a.exe \
# DEFINE:     | FileCheck --check-prefix=IMAGE %s && \
# DEFINE:   not %{link} main.obj rt.lib -stub:a.exe 2>&1 \
# DEFINE:     | FileCheck --check-prefix=STUB %s && \
# DEFINE:   not %{link} key.obj rt.lib 2>&1 \
# DEFINE:     | FileCheck --check-prefix=VARIANT %s && \
# DEFINE:   not %{link} main.obj unnamed.obj rt.lib 2>&1 \
# DEFINE:     | FileCheck --check-prefix=UNNAMED %s && \
# DEFINE:   %{link} hidden.obj rt.lib && od -v -A x -t x4 -N 0x80 a.exe > none.txt && \
# DEFINE:   llvm-readobj --sections --coff-imports a.exe >> none.txt && \
# DEFINE:   FileCheck --input-file=none.txt --check-prefix=NONE %s \
# DEFINE:     --implicit-check-not=.ovra --implicit-check-not="Symbol: new (" && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn a.exe \
# DEFINE:     | FileCheck --check-prefix=HIDDEN-%{a} %s && \
# DEFINE:   %{link} none.obj rt.lib && od -v -A x -t x4 -N 0x80 a.exe > none.txt && \
# DEFINE:   llvm-readobj --sections a.exe >> none.txt && \
# DEFINE:   FileCheck --input-file=none.txt --check-prefix=NONE %s \
# DEFINE:     --implicit-check-not=.ovra

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}
# REDEFINE: %{a} = x64
# REDEFINE: %{t} = x86_64
# REDEFINE: %{e} = ntposix
# RUN: %{run}

## The DOS header says that it takes twelve paragraphs, where its offer table
## is, which the anchor states too, and that the PE header is at 0xf8. Slots 0 and 8 are replaced, slot 1 forwards to slot 0, slots
## 9 and 10 to slot 8 across a dropped last argument and slot 11 across a
## dropped middle one, and the table has twelve slots.
# TABLE:      [[#%x,DELA:]] T dela
# TABLE:      [[#%x,NEW:]] T new
# TABLE:      000000 {{[0-9a-f]+}} {{[0-9a-f]+}} 0000000c
# TABLE:      000030 00000000 00000000 {{[0-9a-f]+}} 000000f8
# TABLE-NEXT: 000040 c5e1a7f3 [[#%.8x,NEW-0x140000000]] c5e1a7f2 [[#%.8x,NEW-0x140000000]]
# TABLE-NEXT: 000050 00000000 00000000 00000000 00000000
# TABLE-NEXT: 000060 00000000 00000000 00000000 00000000
# TABLE-NEXT: 000070 00000000 00000000 00000000 00000000
# TABLE-NEXT: 000080 7b3ac2d5 000c0001 [[#%.8x,ANCHOR:]] 00000000
# TABLE-NEXT: 000090 00000000 00000000 c5e1a7fb [[#%.8x,DELA-0x140000000]]
# TABLE-NEXT: 0000a0 c5e1a7fa [[T:[0-9a-f]+]] c5e1a7f9 [[T]]
# TABLE-NEXT: 0000b0 c5e1a7f8 [[#%.8x,M:]] 00000000 00000000
# TABLE-NEXT: 0000c0 0eba1f0e cd09b400
# TABLE:      Contents of section .ovra:
# TABLE-NEXT:  [[#%x,ANCHOR+0x140000000]] d5c23a7b 01000c00 01010000 {{[0-9a-f]+}}
# TABLE:       0808080b 00000000

## The thunks' prefixes end where their code begins: the pattern of the key's
## marker, then the type ID, without the tag that states delsm's thread-pointer
## entry.
# TEXT:      0f1f8006 5a1c00b8 33333333 {{[^ ]+$}}
# TEXT:      0f1f8006 5a1c00b8 44444444 {{[^ ]+$}}

## The executable's calls reach the replacements and the thunk of the middle
## drop, and its address-takes the replacement and the import of a forwarded
## default.
# x64-itanium:      <main>:
# x64-itanium-NEXT: callq 0x[[#%x,NEW:]] <new>
# x64-itanium-NEXT: callq 0x[[#NEW]] <new>
# x64-itanium-NEXT: callq 0x[[#%x,DELA:]] <dela>
# x64-itanium-NEXT: callq 0x[[#%x,M:]]
# x64-itanium-NEXT: leaq {{.*}} <new>
# x64-itanium-NEXT: movq {{.*}} <__imp_newa>
# x64-itanium:      jmp 0x[[#DELA]] <dela>
# x64-itanium:      [[#M]]: movq %r8, %rdx
# x64-itanium-NEXT: jmp 0x[[#DELA]] <dela>

# x64-ntposix:      <main>:
# x64-ntposix-NEXT: callq 0x[[#%x,NEW:]] <new>
# x64-ntposix-NEXT: callq 0x[[#NEW]] <new>
# x64-ntposix-NEXT: callq 0x[[#%x,DELA:]] <dela>
# x64-ntposix-NEXT: callq 0x[[#%x,M:]]
# x64-ntposix:      [[#M]]: movq %rdx, %rsi
# x64-ntposix-NEXT: jmp 0x[[#DELA]] <dela>

# arm64-itanium:      <main>:
# arm64-itanium-NEXT: bl 0x[[#%x,NEW:]] <new>
# arm64-itanium-NEXT: bl 0x[[#NEW]] <new>
# arm64-itanium-NEXT: bl 0x[[#%x,DELA:]] <dela>
# arm64-itanium-NEXT: bl 0x[[#%x,M:]]
# arm64-itanium:      b 0x[[#DELA]] <dela>
# arm64-itanium-NEXT: brk #0
# arm64-itanium-NEXT: brk #0
# arm64-itanium:      [[#M]]: mov x1, x2
# arm64-itanium-NEXT: b 0x[[#DELA]] <dela>

## Each address of the anchor is relocated, and each is a call target, the
## shared thunk once. The executable imports only the default whose address
## it takes, and exports the replacement.
# IMAGE:      Name: .ovra
# IMAGE-NEXT: VirtualSize: 0x88
# IMAGE-NEXT: VirtualAddress: 0x[[#%X,ANCHOR:]]
# IMAGE:        IMAGE_SCN_CNT_INITIALIZED_DATA
# IMAGE-NEXT:   IMAGE_SCN_MEM_READ
# IMAGE-NEXT: ]
# IMAGE:      Import {
# IMAGE-NEXT:   Name: rt.dll
# IMAGE:        Symbol: newa
# IMAGE-NEXT: }
# IMAGE:      Export {
# IMAGE-NEXT:   Ordinal: 1
# IMAGE-NEXT:   Name: new
# IMAGE-NEXT:   RVA: 0x
# IMAGE:      BaseReloc [
# IMAGE:        Address: 0x[[#%X,ANCHOR+0x18]]
# IMAGE:        Address: 0x[[#%X,ANCHOR+0x20]]
# IMAGE:        Address: 0x[[#%X,ANCHOR+0x58]]
# IMAGE:        Address: 0x[[#%X,ANCHOR+0x60]]
# IMAGE:        Address: 0x[[#%X,ANCHOR+0x68]]
# IMAGE:        Address: 0x[[#%X,ANCHOR+0x70]]
# IMAGE:      GuardFidTable [
# IMAGE-NEXT:   0x
# IMAGE-NEXT:   0x
# IMAGE-NEXT:   0x
# IMAGE-NEXT:   0x
# IMAGE-NEXT: ]

# STUB: error: /stub: the executable replaces an overridable default, and its override table takes the place of the DOS stub

# VARIANT: error: the type prefix of overridable default delsa is of the variant 1C5A06.0, and the executable's of 1C5A06.4

# UNNAMED: error: unnamed.obj: other replaces an overridable default whose slot no input states; link the import library of the DLL that defines the default

## A hidden replacement is what the executable's references bind, and there
## is no override table: the DOS program follows the header, which names the
## offer table.
# HIDDEN-x64:        <main>:
# HIDDEN-x64-NEXT:   callq 0x{{[0-9a-f]+}} <new>
# HIDDEN-arm64:      <main>:
# HIDDEN-arm64-NEXT: bl 0x{{[0-9a-f]+}} <new>

# NONE:      000030 00000000 00000000 {{[0-9a-f]+}} 00000078
# NONE-NEXT: 000040 0eba1f0e

#--- rt.s
        .section .drectve,"yn"
        .ascii " -export:new -export:newa -export:nothrow -export:dela"
        .ascii " -export:delsa -export:delsa2 -export:delsm"
        .ascii " -failifmismatch:_WIN_ITANIUM_KCFI_PREFIX=1C5A06.0"
        .macro DEFAULT name, id
        .p2align 4
        .long 0x06801f0f, 0xb8001c5a, \id
        .weak \name
\name:
        ret
        .link_fact \name, 0, 0
        .endm
        .macro TPDEFAULT name, id
        .p2align 4
        .long \name\()$tp - . - 16, 0x06821f0f, 0xb8001c5a, \id | 0x80000000
        .weak \name
\name:
        ret
\name\()$tp:
        ret
        .link_fact \name, 0, 2
        .endm
        .text
        DEFAULT new, 0x11111111
        DEFAULT newa, 0x11111111
        DEFAULT nothrow, 0x55555555
        DEFAULT dela, 0x22222222
        DEFAULT delsa, 0x33333333
        DEFAULT delsa2, 0x33333333
        TPDEFAULT delsm, 0x44444444
        .link_fact new, 9, 0
        .link_fact newa, 9, 1, 0
        .link_fact nothrow, 9, 4
        .link_fact dela, 9, 8
        .link_fact delsa, 9, 9, 8, 1
        .link_fact delsa2, 9, 10, 9
        .link_fact delsm, 9, 11, 8, 2

#--- main.s
        .section .drectve,"yn"
.ifdef KEY
        .ascii " -failifmismatch:_WIN_ITANIUM_KCFI_PREFIX=1C5A06.4"
.endif
.ifndef NONE
.ifndef HIDDEN
        .ascii " -export:new"
.endif
.endif
        .text
        .globl main
main:
.ifndef NONE
.ifdef x64
        callq *__imp_new(%rip)
        callq *__imp_newa(%rip)
        callq *__imp_delsa(%rip)
        callq *__imp_delsm(%rip)
        leaq new(%rip), %rax
        movq __imp_newa(%rip), %rax
.else
        adrp x16, __imp_new
        ldr x16, [x16, :fused_lo12:__imp_new]
        .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_new
        blr x16
        bl newa
        bl delsa
        bl delsm
.endif
.endif
        ret
.ifndef NONE
        .globl new
        .p2align 4
new:
        ret
        .globl dela
        .p2align 4
dela:
        ret
.ifdef HIDDEN
        .link_fact new, 1, 3
        .link_fact dela, 1, 3
.endif
        .link_fact new, 9
        .link_fact dela, 9
.endif
        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .fill 112, 1, 0

#--- unnamed.s
        .text
        .globl other
other:
        ret
        .link_fact other, 9
