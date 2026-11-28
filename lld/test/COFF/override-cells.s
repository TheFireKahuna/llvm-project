# REQUIRES: x86, aarch64

## Every word that holds the address of an overridable default is written by
## its image's fill from the executable's override table, as the program
## chose. The fill's list counts them in its header and names each, a cell,
## as a word of kind 4 before any other, with its use in the two bits above
## the kind and its slot above bit 8: 0 for a call word, which takes what a
## call of the default reaches, 1 for an address word, which takes the
## default's address in every image, and 2 for a default's check word; and
## for a word of the image's own, the default, whose address the word takes
## where the table names nothing.
##
## An image that defines a default checks the default's own word, its local
## import pointer, which is in .sealed, and its own calls and jumps of the
## default, direct or through the pointer, reach the default's own entry,
## which reads the word too. Its address-takes and other reads of the
## pointer read a word of the default's address, also in .sealed, and so does
## the fill for a word of data that holds the address, and so does a label
## that the assembler makes for the default's place; but a local symbol that
## names the default takes the default's own address, as on ELF. A
## replacement that is the image's alone, of hidden visibility, is what the
## words of the defaults that forward to it hold, which no entry names.
##
## A DLL's entry of a default's import is an address word, as is a word of
## data that the loader binds, and it calls a default through that entry;
## one that forwards to another it calls through a second import of it, a
## call word. An executable has none: its link binds its references from its
## table, and its address-takes of a default that it defines take the default.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{dll} = lld-link -machine:%{a} -dll -noentry fill.lib
# DEFINE: %{run} = \
# DEFINE:   %{mc} --defsym=%{a}=1 rt.s -o rt.obj && %{mc} mine.s -o mine.obj && \
# DEFINE:   %{mc} --defsym=%{a}=1 user.s -o user.obj && \
# DEFINE:   %{mc} --defsym=%{a}=1 fill.s -o fill.obj && \
# DEFINE:   rm -f fill.lib && llvm-ar rcs fill.lib fill.obj && \
# DEFINE:   %{dll} rt.obj -out:rt.dll -implib:rt.lib && \
# DEFINE:   llvm-readobj --sections rt.dll > rt.txt && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn rt.dll >> rt.txt && \
# DEFINE:   llvm-objdump -s -j .rdata -j .sealed rt.dll >> rt.txt && \
# DEFINE:   FileCheck --check-prefixes=RT,RT-%{a} %s < rt.txt && \
# DEFINE:   %{dll} mine.obj rt.obj -out:mine.dll && \
# DEFINE:   llvm-objdump -s -j .rdata mine.dll \
# DEFINE:     | FileCheck --check-prefix=MINE %s && \
# DEFINE:   %{dll} user.obj rt.lib -out:user.dll -debug:symtab -opt:ref && \
# DEFINE:   llvm-readobj --coff-imports user.dll > user.txt && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn user.dll >> user.txt && \
# DEFINE:   llvm-objdump -s -j .rdata -j .sealed user.dll >> user.txt && \
# DEFINE:   FileCheck --check-prefixes=USER,USER-%{a} %s < user.txt && \
# DEFINE:   %{mc} tl.s -o tl.obj && %{dll} tl.obj rt.lib -out:tl.dll && \
# DEFINE:   llvm-objdump -s -j .rdata -j .tls tl.dll \
# DEFINE:     | FileCheck --check-prefix=TL %s && \
# DEFINE:   lld-link -machine:%{a} -entry:inner -subsystem:console fill.lib \
# DEFINE:     rt.obj -out:static.exe && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn static.exe \
# DEFINE:     | FileCheck --check-prefix=STATIC-%{a} %s && \
# DEFINE:   lld-link -machine:%{a} -entry:user -subsystem:console fill.lib \
# DEFINE:     user.obj rt.lib -out:user.exe && \
# DEFINE:   llvm-readobj --sections --coff-imports user.exe \
# DEFINE:     | FileCheck --check-prefix=EXE %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## The list names no range and eight words, seven cells and one word of data;
## .sealed holds the done word, the check words of slots 0 to 3, the address
## words of slots 1 to 3, and the table, which the fill computes from slot
## 1's.
# RT:          Name: .sealed
# RT-NEXT:     VirtualSize: 0x48
# RT-NEXT:     VirtualAddress: 0x3000
# RT-x64:      <inner>:
# RT-x64-NEXT: callq 0x180001034
# RT-x64-NEXT: callq 0x18000101a
# RT-x64-NEXT: movq {{.*}} # 0x180003030
# RT-x64-NEXT: movq {{.*}} # 0x180003038
# RT-x64-NEXT: leaq {{.*}} <del>
# RT-arm64:      <inner>:
# RT-arm64-NEXT: bl 0x180001028
# RT-arm64-NEXT: ldr x0, 0x180003030
# RT-arm64-NEXT: ldr x0, 0x180003038
# RT-arm64-NEXT: adr x0, {{.*}} <del>
# RT-arm64-NEXT: ldr x0, 0x180003030
# RT:      Contents of section .rdata:
# RT-NEXT: 180002000 00000000 07000000 08000000 00000000
# RT-NEXT: 180002010 00300000 48000000 00000000 00000000
# RT-NEXT: 180002020 08300000 14000000 [[NEW:[0-9a-f]+]] 00000000
# RT-NEXT: 180002030 10300000 14010000 [[NEWA:[0-9a-f]+]] 00000000
# RT-NEXT: 180002040 18300000 14020000 [[DEL:[0-9a-f]+]] 00000000
# RT-NEXT: 180002050 20300000 14030000 [[DELSZ:[0-9a-f]+]] 00000000
# RT-NEXT: 180002060 28300000 0c010000 [[NEWA]] 00000000
# RT-NEXT: 180002070 30300000 0c020000 [[DEL]] 00000000
# RT-NEXT: 180002080 38300000 0c030000 [[DELSZ]] 00000000
# RT-NEXT: 180002090 40300000 28300000
# RT:      Contents of section .sealed:
# RT-NEXT: 180003000 00000000 00000000

## Slots 0 and 1 have no words of their own: newa's pointer holds new, the
## first function of the image.
# MINE:      180002000 00000000 05000000 06000000 00000000
# MINE-NEXT: 180002010 00300000 38000000 00000000 00000000
# MINE-NEXT: 180002020 08300000 14020000
# MINE:      00100080 01000000

## The second imports of delsz and newa, which forward, are call words; the
## entries of new and newa address words, as is del's word of data. delsz's
## own entry, which nothing reads, is not imported. The list names the range
## of the entries.
# USER:      Name: rt.dll
# USER-NEXT: ImportLookupTableRVA:
# USER-NEXT: ImportAddressTableRVA: 0x2000
# USER-NEXT: Symbol: delsz (0)
# USER-NEXT: Symbol: newa (0)
# USER-NEXT: Symbol: new (3)
# USER-NEXT: Symbol: newa (4)
# USER:      Name: rt.dll
# USER-NEXT: ImportLookupTableRVA:
# USER-NEXT: ImportAddressTableRVA: 0x4000
# USER-NEXT: Symbol: del (0)
# USER-x64:      <user>:
# USER-x64-NEXT: callq *{{.*}} # 0x180002010
# USER-x64-NEXT: callq *{{.*}} # 0x180002000
# USER-x64-NEXT: callq *{{.*}} # 0x180002008
# USER-x64-NEXT: movq {{.*}} # 0x180002018
# USER-arm64:      <user>:
# USER-arm64-NEXT: ldr x16, 0x180002010
# USER-arm64-NEXT: blr x16
# USER-arm64-NEXT: ldr x16, 0x180002000
# USER-arm64-NEXT: blr x16
# USER-arm64-NEXT: ldr x16, 0x180002008
# USER-arm64-NEXT: blr x16
# USER-arm64-NEXT: ldr x0, 0x180002018
# USER:      180002020 00000000 00000000 01000000 05000000
# USER-NEXT: 180002030 05000000 00000000 00300000 08000000
# USER-NEXT: 180002040 00000000 00000000 00200000 20000000
# USER-NEXT: 180002050 00200000 04030000 00000000 00000000
# USER-NEXT: 180002060 08200000 04010000 00000000 00000000
# USER-NEXT: 180002070 10200000 0c000000 00000000 00000000
# USER-NEXT: 180002080 18200000 0c010000 00000000 00000000
# USER-NEXT: 180002090 00400000 0c020000

## A word of thread-local data that holds a default's address is no slot,
## which the loader would bind in the template alone: the fill writes it, the
## last of its list, from the entry, which is a cell, and it holds no pointer
## until then.
# TL:      180002030 00000000 00001000 01000000 01000000
# TL-NEXT: 180002040 02000000 01000000 00300000 08000000
# TL-NEXT: 180002050 00000000 00000000 00200000 08000000
# TL-NEXT: 180002060 00200000 0c020000 00000000 00000000
# TL-NEXT: 180002070 00500000 00200000 00000000 00000000
# TL:      Contents of section .tls:
# TL-NEXT: 180005000 f1f1f1f1 f1f1f1f1
# STATIC-x64:      <inner>:
# STATIC-x64-NEXT: callq
# STATIC-x64-NEXT: callq
# STATIC-x64-NEXT: leaq {{.*}} <del>
# STATIC-x64-NEXT: movq
# STATIC-x64-NEXT: leaq {{.*}} <del>
# STATIC-arm64:      <inner>:
# STATIC-arm64-NEXT: bl
# STATIC-arm64-NEXT: adr x0, {{.*}} <del>
# STATIC-arm64-NEXT: ldr x0,
# STATIC-arm64-NEXT: adr x0, {{.*}} <del>
# STATIC-arm64-NEXT: adr x0, {{.*}} <del>

# EXE-NOT:  Name: .sealed
# EXE:      Name: rt.dll
# EXE-NOT:  Name: ntdll.dll

#--- rt.s
        .section .drectve,"yn"
        .ascii " -export:new -export:newa -export:del -export:delsz"
        .ascii " -export:inner -export:table"
        .ascii " -failifmismatch:_WIN_ITANIUM_KCFI_PREFIX=1C5A06.0"
        # A default checks its word, and its own entry enters it where the
        # word holds it and jumps through the word otherwise.
        .macro DEFAULT name, id
        .section .text,"xr",one_only,\name
        .p2align 4
        .long 0x06801f0f, 0xb8001c5a, \id
        .weak \name
\name:
.L\name\()_at:
.ifdef x64
        leaq \name(%rip), %r11
        cmpq %r11, __imp_\name(%rip)
        jne 1f
        retq
1:      jmpq *__imp_\name(%rip)
.else
        adrp x16, __imp_\name
        ldr x16, [x16, :lo12:__imp_\name]
        adr x17, \name
        cmp x16, x17
        b.ne 1f
        ret
1:      br x16
.endif
        .section .text,"xr",one_only,__ovr_own$\name
        .globl __ovr_own$\name
__ovr_own$\name:
.ifdef x64
        leaq \name(%rip), %r11
        cmpq %r11, __imp_\name(%rip)
        je \name
        jmpq *__imp_\name(%rip)
.else
        adrp x16, __imp_\name
        ldr x16, [x16, :lo12:__imp_\name]
        adr x17, \name
        cmp x16, x17
        b.eq \name
        br x16
.endif
        .link_fact \name, 0, 0
        .endm
        DEFAULT new, 0x11111111
        DEFAULT newa, 0x11111111
        DEFAULT del, 0x22222222
        DEFAULT delsz, 0x33333333
        .link_fact new, 9, 0
        .link_fact newa, 9, 1, 0
        .link_fact del, 9, 2
        .link_fact delsz, 9, 3, 2, 1

        .section .text,"xr",one_only,inner
        .globl inner
inner:
.ifdef x64
        callq del
        callq *__imp_newa(%rip)
        leaq del(%rip), %rax
        movq __imp_delsz(%rip), %rax
        leaq own_del(%rip), %rax
        retq
.else
        bl del
        adrp x0, del
        add x0, x0, :lo12:del
        adrp x0, __imp_delsz
        ldr x0, [x0, :lo12:__imp_delsz]
        adrp x0, own_del
        add x0, x0, :lo12:own_del
        adrp x0, .Ldel_at
        add x0, x0, :lo12:.Ldel_at
        ret
.endif
        own_del = del
        .section .rdata,"dr"
        .globl table
table:  .quad newa

#--- mine.s
        .text
        .globl new
new:
        ret
        .link_fact new, 1, 3
        .link_fact new, 9

#--- user.s
        .text
        .globl user
user:
.ifdef x64
        callq *__imp_new(%rip)
        callq *__imp_delsz(%rip)
        callq *__imp_newa(%rip)
        movq __imp_newa(%rip), %rax
        retq
.else
        adrp x16, __imp_new
        ldr x16, [x16, :fused_lo12:__imp_new]
        .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_new
        blr x16
        adrp x16, __imp_delsz
        ldr x16, [x16, :fused_lo12:__imp_delsz]
        .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_delsz
        blr x16
        adrp x16, __imp_newa
        ldr x16, [x16, :fused_lo12:__imp_newa]
        .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_newa
        blr x16
        adrp x0, __imp_newa
        ldr x0, [x0, :lo12:__imp_newa]
        ret
.endif
        .data
        .quad del

#--- fill.s
## The fill's entries, which name the list and the entry they run after it.
.macro entry name
.section .text,"xr",one_only,\name
.globl \name
\name:
.ifdef x64
  leaq __llvm_fill_list(%rip), %rax
.else
  adrp x0, __llvm_fill_list
.endif
  ret
  .rva __llvm_fill_target
.endm
  entry __llvm_fill_dll
  entry __llvm_fill_dll_imports
.section .text,"xr",one_only,__llvm_fill_true
.globl __llvm_fill_true
__llvm_fill_true:
  ret

#--- tl.s
        .text
        .globl tl
tl:
        ret
        .section .tls$,"dw"
        .quad del
        .section .rdata,"dr"
        .globl _tls_used
_tls_used:
        .fill 40, 1, 0
