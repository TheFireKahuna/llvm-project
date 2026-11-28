# REQUIRES: x86, aarch64

## An imported function has no address in the image for an object for Windows
## Itanium or NT-POSIX, as a shared object's function has none for lld/ELF. A
## reference to one by its own name has a value only where something provides
## it: the thunk, for a direct call or jump with no addend and for a call-only
## field; the import's entry, for code that takes the address, which then
## loads it; the loader, for a whole word of data, which it binds in place.
## Any other reference is one error, whatever its type, so a type has no value
## against an imported function until it is given a provider. A section the
## image does not load, a delay-loaded function and another object's
## references keep the thunk, except an x86-64 field before an immediate.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{link} = lld-link -dll -noentry -include:main -errorlimit:0
# DEFINE: %{x64} = llvm-mc -filetype=obj -triple=x86_64-windows-itanium
# DEFINE: %{a64} = llvm-mc -filetype=obj -triple=aarch64-windows-itanium
# RUN: lld-link -machine:x64 -def:imp.def -out:imp.lib
# RUN: lld-link -machine:arm64 -def:imp.def -out:imp-a64.lib

## Every relocation type, one of each against fn in code and the fields of
## data: the ones not listed here have a provider.
# RUN: yaml2obj all.yaml -o all.obj
# RUN: yaml2obj all-a64.yaml -o all-a64.obj
# RUN: not %{link} imp.lib all.obj -out:all.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=ALL --implicit-check-not="needs an address"
# RUN: not %{link} imp-a64.lib all-a64.obj -out:all.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=ALL-A64 --implicit-check-not="needs an address"

# ALL: error: all.obj: the IMAGE_REL_AMD64_ADDR32 relocation at offset 0x3 of .text needs an address for fn, which is imported from imp.dll and has none in this image; call or jump to fn, or use its pointer, __imp_fn, in assembly fn@GOTPCREL(%rip)
# ALL-NEXT: IMAGE_REL_AMD64_ADDR32NB relocation at offset 0xb of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_REL32 relocation at offset 0x13 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_REL32_1 relocation at offset 0x1b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_REL32_2 relocation at offset 0x23 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_REL32_3 relocation at offset 0x2b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_REL32_4 relocation at offset 0x33 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_REL32_5 relocation at offset 0x3b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_SECTION relocation at offset 0x43 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_SECREL relocation at offset 0x4b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_SECREL7 relocation at offset 0x53 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_TOKEN relocation at offset 0x5b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_SREL32 relocation at offset 0x63 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_EHANDLER relocation at offset 0x6b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_MEM_CALL relocation at offset 0xa3 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_MEM_JUMP relocation at offset 0xab of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_MEM_JUMP_P1 relocation at offset 0xb3 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_MEM_JUMP_TAIL relocation at offset 0xbb of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_MEM_LOAD relocation at offset 0xc3 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_MEM_LOAD_REX2 relocation at offset 0xcb of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_LEA_NARROW relocation at offset 0xe3 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_FUSED_READ relocation at offset 0x103 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_FUSED_READ_REX2 relocation at offset 0x10b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_FUSED_ACCESS relocation at offset 0x113 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_FUSED_INDEX relocation at offset 0x11b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_FUSED_TEST relocation at offset 0x123 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_TPOFF32 relocation at offset 0x12b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_SSPAN32 relocation at offset 0x133 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_SSPAN_EPILOGV2 relocation at offset 0x13b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_SSPAN8 relocation at offset 0x143 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_SSPAN16 relocation at offset 0x14b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_SSPAN64 relocation at offset 0x153 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_SSPAN_ULEB relocation at offset 0x15b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_SSPAN_SLEB relocation at offset 0x163 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_USPAN8 relocation at offset 0x16b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_LLVM_USPAN16 relocation at offset 0x173 of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_PAIR relocation at offset 0x17b of .text needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_ADDR32 relocation at offset 0x8 of .data needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_ADDR32NB relocation at offset 0xc of .data needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_REL32 relocation at offset 0x10 of .data needs an address for fn
# ALL-NEXT: IMAGE_REL_AMD64_SECREL relocation at offset 0x14 of .data needs an address for fn
# ALL-A64: error: all-a64.obj: the IMAGE_REL_ARM64_ADDR32 relocation at offset 0x0 of .text needs an address for fn, which is imported from imp.dll and has none in this image; call or jump to fn, or use its pointer, __imp_fn, in assembly :got:fn and :got_lo12:fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_ADDR32NB relocation at offset 0x8 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_PAGEBASE_REL21 relocation at offset 0x18 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_REL21 relocation at offset 0x20 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_PAGEOFFSET_12A relocation at offset 0x28 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_PAGEOFFSET_12L relocation at offset 0x30 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_SECREL relocation at offset 0x38 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_SECREL_LOW12A relocation at offset 0x40 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_SECREL_HIGH12A relocation at offset 0x48 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_SECREL_LOW12L relocation at offset 0x50 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_TOKEN relocation at offset 0x58 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_SECTION relocation at offset 0x60 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_REL32 relocation at offset 0x78 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G0 relocation at offset 0x98 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G0_NC relocation at offset 0xa0 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G1 relocation at offset 0xa8 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_LOGIMM relocation at offset 0xb0 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL relocation at offset 0xb8 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL relocation at offset 0xc0 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_FUSED_READ relocation at offset 0xe0 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_FUSED_TEST relocation at offset 0xe8 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_TPOFF_HIGH12A relocation at offset 0xf0 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_TPOFF_LOW12A relocation at offset 0xf8 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_TPOFF_LOW12L relocation at offset 0x100 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_SSPAN8 relocation at offset 0x108 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_SSPAN16 relocation at offset 0x110 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_SSPAN32 relocation at offset 0x118 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_SSPAN64 relocation at offset 0x120 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_SSPAN_ULEB relocation at offset 0x128 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_SSPAN_SLEB relocation at offset 0x130 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_SSPAN_WORDS18 relocation at offset 0x138 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_USPAN8 relocation at offset 0x140 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_USPAN16 relocation at offset 0x148 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_USPAN8_W relocation at offset 0x150 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_LLVM_USPAN16_W relocation at offset 0x158 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_PAIR relocation at offset 0x160 of .text needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_ADDR32 relocation at offset 0x8 of .data needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_ADDR32NB relocation at offset 0xc of .data needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_REL32 relocation at offset 0x10 of .data needs an address for fn
# ALL-A64-NEXT: IMAGE_REL_ARM64_SECREL relocation at offset 0x14 of .data needs an address for fn
## What assembly ported from ELF writes: calls and jumps reach the thunk, the
## address is the function's own however it is taken, and a call-only field
## holds the thunk.
# RUN: %{x64} good.s -o good.obj
# RUN: %{link} imp.lib good.obj -out:good.dll
# RUN: llvm-objdump -d good.dll > good.txt
# RUN: llvm-objdump -s -j .data good.dll >> good.txt
# RUN: llvm-readobj --coff-basereloc good.dll >> good.txt
# RUN: FileCheck --check-prefix=GOOD %s < good.txt

# GOOD:      180001000: e8 {{.*}} callq 0x180001020
# GOOD-NEXT: e8 {{.*}} callq 0x180001020
# GOOD-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,IAT:]]
# GOOD-NEXT: 48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#IAT]]
# GOOD-NEXT: jne 0x180001020
# GOOD-NEXT: jmp 0x180001020
# GOOD:      180001020: ff 25 {{.*}} jmpq *{{.*}}(%rip) {{.*}}# 0x[[#IAT]]
## The word is the loader's, with the import's name until it is bound.
# GOOD:      Contents of section .data:
# GOOD-NEXT: 180003000 {{[0-9a-f]*[1-9a-f][0-9a-f]*}} 00000000 20100000 14e0ffff
# GOOD:      BaseReloc [
# GOOD-NEXT: ]

# RUN: %{a64} good-a64.s -o good-a64.obj
# RUN: %{link} imp-a64.lib good-a64.obj -out:good-a64.dll
# RUN: llvm-objdump -d good-a64.dll > good-a64.txt
# RUN: llvm-objdump -s -j .data good-a64.dll >> good-a64.txt
# RUN: llvm-readobj --coff-basereloc good-a64.dll >> good-a64.txt
# RUN: FileCheck --check-prefix=GOOD-A64 %s < good-a64.txt

# GOOD-A64:      180001000: {{.*}} bl 0x18000101c
# GOOD-A64-NEXT: ldr x0, 0x[[#%x,IAT:]]
# GOOD-A64-NEXT: adrp x1, 0x[[#%x,PAGE:]]
# GOOD-A64-NEXT: ldr x1, [x1, #0x[[#IAT-PAGE]]]
# GOOD-A64-NEXT: cbz x0, 0x18000101c
# GOOD-A64-NEXT: tbz w0, #0x0, 0x18000101c
# GOOD-A64-NEXT: b 0x18000101c
# GOOD-A64-NEXT: 18000101c: {{.*}} adrp x16, 0x[[#PAGE]]
# GOOD-A64-NEXT: ldr x16, [x16, #0x[[#IAT-PAGE]]]
# GOOD-A64-NEXT: br x16
# GOOD-A64:      Contents of section .data:
# GOOD-A64-NEXT: 180003000 {{[0-9a-f]*[1-9a-f][0-9a-f]*}} 00000000 1c100000 10e0ffff
# GOOD-A64:      BaseReloc [
# GOOD-A64-NEXT: ]

## What it must not write: a read of, or a call or jump through, the memory at
## the function, a branch past it, and an address in a field that cannot load
## it or an add that is not proven to follow its adrp.
# RUN: %{x64} bad.s -o bad.obj
# RUN: not %{link} imp.lib bad.obj -out:bad.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=BAD --implicit-check-not="needs an address"
# BAD:      IMAGE_REL_LLVM_MEM_LOAD relocation at offset 0x3 of .text needs an address for ext
# BAD-NEXT: IMAGE_REL_LLVM_MEM_CALL relocation at offset 0x9 of .text needs an address for ext
# BAD-NEXT: IMAGE_REL_LLVM_MEM_JUMP_TAIL relocation at offset 0xf of .text needs an address for ext
# BAD-NEXT: IMAGE_REL_LLVM_BRANCH32 relocation at offset 0x14 of .text needs an address for ext
# BAD-NEXT: IMAGE_REL_AMD64_REL32_1 relocation at offset 0x1a of .text needs an address for ext
# BAD-NEXT: IMAGE_REL_LLVM_LEA_NARROW relocation at offset 0x21 of .text needs an address for ext
# BAD-NEXT: IMAGE_REL_LLVM_LEA_NARROW relocation at offset 0x2c of .text needs an address for ext
# BAD-NEXT: IMAGE_REL_AMD64_ADDR32NB relocation at offset 0x0 of .data needs an address for ext
# BAD-NEXT: IMAGE_REL_AMD64_REL32 relocation at offset 0x4 of .data needs an address for ext

# RUN: %{a64} bad-a64.s -o bad-a64.obj
# RUN: not %{link} imp-a64.lib bad-a64.obj -out:bad.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=BAD-A64 --implicit-check-not="needs an address"
# BAD-A64:      IMAGE_REL_ARM64_PAGEBASE_REL21 relocation at offset 0x0 of .text needs an address for ext
# BAD-A64-NEXT: IMAGE_REL_ARM64_PAGEOFFSET_12L relocation at offset 0x4 of .text needs an address for ext
# BAD-A64-NEXT: IMAGE_REL_ARM64_REL21 relocation at offset 0x8 of .text needs an address for ext
# BAD-A64-NEXT: IMAGE_REL_ARM64_PAGEBASE_REL21 relocation at offset 0xc of .text needs an address for ext
# BAD-A64-NEXT: IMAGE_REL_ARM64_PAGEOFFSET_12A relocation at offset 0x14 of .text needs an address for ext
# BAD-A64-NEXT: IMAGE_REL_ARM64_PAGEBASE_REL21 relocation at offset 0x18 of .text needs an address for ext
# BAD-A64-NEXT: IMAGE_REL_ARM64_PAGEOFFSET_12A relocation at offset 0x1c of .text needs an address for ext

## A fused read that stays a load reads what is at the function, as a load
## does.
# RUN: %{a64} fused-a64.s -o fused-a64.obj
# RUN: not %{link} imp-a64.lib fused-a64.obj -out:bad.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=FUSED-A64 --implicit-check-not="needs an address"
# FUSED-A64:      IMAGE_REL_ARM64_PAGEBASE_REL21 relocation at offset 0x0 of .text needs an address for ext
# FUSED-A64-NEXT: IMAGE_REL_LLVM_FUSED_READ relocation at offset 0x4 of .text needs an address for ext

## The same references to a delay-loaded function keep its thunk, as do those
## of an object that LLVM did not make for these environments but for the
## field before an immediate, whose instruction reads its target, and those of
## a section the image does not load.
# RUN: %{link} imp.lib bad.obj -out:delay.dll -delayload:imp.dll \
# RUN:   -alternatename:__delayLoadHelper2=main
# RUN: llvm-objdump -d delay.dll | FileCheck --check-prefix=KEPT %s
# RUN: %{link} imp-a64.lib bad-a64.obj -out:delay-a64.dll -delayload:imp.dll \
# RUN:   -alternatename:__delayLoadHelper2=main
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc bad.s -o foreign.obj
# RUN: %{x64} debug.s -o debug.obj
# RUN: not %{link} imp.lib foreign.obj debug.obj -out:foreign.dll 2>&1 | \
# RUN:   FileCheck %s --check-prefix=FOREIGN \
# RUN:   --implicit-check-not="needs an address"
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc bad-a64.s \
# RUN:   -o foreign-a64.obj
# RUN: %{a64} debug.s -o debug-a64.obj
# RUN: %{link} imp-a64.lib foreign-a64.obj debug-a64.obj -out:foreign-a64.dll
# RUN: %{link} -debug:dwarf imp.lib good.obj debug.obj -out:debug.dll
# RUN: %{link} -debug:dwarf imp-a64.lib good-a64.obj debug-a64.obj \
# RUN:   -out:debug-a64.dll

# KEPT:      180001000: 48 8b 05 {{.*}} movq {{.*}} # 0x[[#%x,THUNK:]]
# KEPT-NEXT: callq *{{.*}} # 0x[[#THUNK]]
# FOREIGN: error: foreign.obj: the IMAGE_REL_AMD64_REL32_1 relocation at offset 0x1a of .text needs an address for ext

#--- imp.def
LIBRARY imp.dll
EXPORTS
fn
fn$so8
ext

#--- good.s
.text
.globl main
main:
  call ext@PLT
  call ext
  movq ext@GOTPCREL(%rip), %rax
  leaq ext(%rip), %rcx
  jne ext
  jmp ext@PLT
.data
  .quad ext
  .long ext@IMGREL_CALL
  .long ext@REL32_CALL

#--- good-a64.s
.text
.globl main
main:
  bl ext
  adrp x0, :got:ext
  ldr x0, [x0, :got_lo12:ext]
  adrp x1, ext
  add x1, x1, :lo12:ext
  cbz x0, ext
  tbz x0, #0, ext
  b ext
.data
  .quad ext
  .long ext@IMGREL_CALL
  .long ext@REL32_CALL

#--- bad.s
.text
.globl main
main:
  movq ext(%rip), %rax
  call *ext(%rip)
  jmp *ext(%rip)
  call ext+4
  cmpl $1, ext(%rip)
  leal ext(%rip), %eax
## The last byte of the instruction before this lea is that of a REX.W prefix.
  movl $0x48000000, %ecx
  leal ext(%rip), %eax
.data
  .long ext@IMGREL
  .long ext-.

#--- bad-a64.s
.text
.globl main
main:
  adrp x0, ext
  ldr x0, [x0, :lo12:ext]
  adr x1, ext
  adrp x2, ext
  nop
  add x2, x2, :lo12:ext
  adrp x3, ext
  add x4, x3, :lo12:ext

#--- fused-a64.s
.text
.globl main
main:
  adrp x5, ext
  ldr x5, [x5, :fused_lo12:ext]

#--- debug.s
.section .debug_info,"drD"
  .secrel32 ext

#--- all.yaml
--- !COFF
header:
  Machine: IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name: .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment: 4
    SectionData: 488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050800000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090488D050000000090
    Relocations:
      - { VirtualAddress: 3, SymbolName: fn, Type: IMAGE_REL_AMD64_ADDR32 }
      - { VirtualAddress: 11, SymbolName: fn, Type: IMAGE_REL_AMD64_ADDR32NB }
      - { VirtualAddress: 19, SymbolName: fn, Type: IMAGE_REL_AMD64_REL32 }
      - { VirtualAddress: 27, SymbolName: fn, Type: IMAGE_REL_AMD64_REL32_1 }
      - { VirtualAddress: 35, SymbolName: fn, Type: IMAGE_REL_AMD64_REL32_2 }
      - { VirtualAddress: 43, SymbolName: fn, Type: IMAGE_REL_AMD64_REL32_3 }
      - { VirtualAddress: 51, SymbolName: fn, Type: IMAGE_REL_AMD64_REL32_4 }
      - { VirtualAddress: 59, SymbolName: fn, Type: IMAGE_REL_AMD64_REL32_5 }
      - { VirtualAddress: 67, SymbolName: fn, Type: IMAGE_REL_AMD64_SECTION }
      - { VirtualAddress: 75, SymbolName: fn, Type: IMAGE_REL_AMD64_SECREL }
      - { VirtualAddress: 83, SymbolName: fn, Type: IMAGE_REL_AMD64_SECREL7 }
      - { VirtualAddress: 91, SymbolName: fn, Type: IMAGE_REL_AMD64_TOKEN }
      - { VirtualAddress: 99, SymbolName: fn, Type: IMAGE_REL_AMD64_SREL32 }
      - { VirtualAddress: 107, SymbolName: fn, Type: IMAGE_REL_AMD64_EHANDLER }
      - { VirtualAddress: 115, SymbolName: fn, Type: IMAGE_REL_LLVM_ADDR32NB_CALL }
      - { VirtualAddress: 123, SymbolName: fn, Type: IMAGE_REL_LLVM_REL32_CALL }
      - { VirtualAddress: 131, SymbolName: fn, Type: IMAGE_REL_LLVM_BRANCH_TAIL }
      - { VirtualAddress: 139, SymbolName: fn, Type: IMAGE_REL_LLVM_BRANCH32 }
      - { VirtualAddress: 147, SymbolName: fn, Type: IMAGE_REL_LLVM_JUMP32 }
      - { VirtualAddress: 155, SymbolName: fn, Type: IMAGE_REL_LLVM_JUMP8 }
      - { VirtualAddress: 163, SymbolName: fn, Type: IMAGE_REL_LLVM_MEM_CALL }
      - { VirtualAddress: 171, SymbolName: fn, Type: IMAGE_REL_LLVM_MEM_JUMP }
      - { VirtualAddress: 179, SymbolName: fn, Type: IMAGE_REL_LLVM_MEM_JUMP_P1 }
      - { VirtualAddress: 187, SymbolName: fn, Type: IMAGE_REL_LLVM_MEM_JUMP_TAIL }
      - { VirtualAddress: 195, SymbolName: fn, Type: IMAGE_REL_LLVM_MEM_LOAD }
      - { VirtualAddress: 203, SymbolName: fn, Type: IMAGE_REL_LLVM_MEM_LOAD_REX2 }
      - { VirtualAddress: 211, SymbolName: fn, Type: IMAGE_REL_LLVM_LEA }
      - { VirtualAddress: 219, SymbolName: fn, Type: IMAGE_REL_LLVM_LEA_OFFSET }
      - { VirtualAddress: 227, SymbolName: fn, Type: IMAGE_REL_LLVM_LEA_NARROW }
      - { VirtualAddress: 259, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_READ }
      - { VirtualAddress: 267, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_READ_REX2 }
      - { VirtualAddress: 275, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_ACCESS }
      - { VirtualAddress: 283, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_INDEX }
      - { VirtualAddress: 291, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_TEST }
      - { VirtualAddress: 299, SymbolName: fn, Type: IMAGE_REL_LLVM_TPOFF32 }
      - { VirtualAddress: 307, SymbolName: fn, Type: IMAGE_REL_AMD64_SSPAN32 }
      - { VirtualAddress: 307, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 315, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN_EPILOGV2 }
      - { VirtualAddress: 315, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 323, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN8 }
      - { VirtualAddress: 323, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 331, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN16 }
      - { VirtualAddress: 331, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 339, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN64 }
      - { VirtualAddress: 339, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 347, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN_ULEB }
      - { VirtualAddress: 347, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 355, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN_SLEB }
      - { VirtualAddress: 355, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 363, SymbolName: fn, Type: IMAGE_REL_LLVM_USPAN8 }
      - { VirtualAddress: 363, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 371, SymbolName: fn, Type: IMAGE_REL_LLVM_USPAN16 }
      - { VirtualAddress: 371, SymbolName: main, Type: IMAGE_REL_AMD64_PAIR }
      - { VirtualAddress: 379, SymbolName: main, Type: IMAGE_REL_AMD64_SSPAN32 }
      - { VirtualAddress: 379, SymbolName: fn, Type: IMAGE_REL_AMD64_PAIR }
  - Name: .data
    Characteristics: [ IMAGE_SCN_CNT_INITIALIZED_DATA, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE ]
    Alignment: 8
    SectionData: '00000000000000000000000000000000000000000000000000000000000000000000000000000000'
    Relocations:
      - { VirtualAddress: 0, SymbolName: fn, Type: IMAGE_REL_AMD64_ADDR64 }
      - { VirtualAddress: 8, SymbolName: fn, Type: IMAGE_REL_AMD64_ADDR32 }
      - { VirtualAddress: 12, SymbolName: fn, Type: IMAGE_REL_AMD64_ADDR32NB }
      - { VirtualAddress: 16, SymbolName: fn, Type: IMAGE_REL_AMD64_REL32 }
      - { VirtualAddress: 20, SymbolName: fn, Type: IMAGE_REL_AMD64_SECREL }
      - { VirtualAddress: 24, SymbolName: fn, Type: IMAGE_REL_LLVM_ADDR32NB_CALL }
      - { VirtualAddress: 28, SymbolName: fn, Type: IMAGE_REL_LLVM_REL32_CALL }
  - Name: .llvm_link_records
    Characteristics: [ IMAGE_SCN_LNK_INFO ]
    Alignment: 1
    SectionData: 4C4C524301000100
symbols:
  - { Name: '@comp.id', Value: 0x4C4C0000, SectionNumber: -1, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_NULL, StorageClass: IMAGE_SYM_CLASS_STATIC }
  - { Name: main, Value: 0, SectionNumber: 1, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_FUNCTION, StorageClass: IMAGE_SYM_CLASS_EXTERNAL }
  - { Name: fn, Value: 0, SectionNumber: 0, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_FUNCTION, StorageClass: IMAGE_SYM_CLASS_EXTERNAL }
...

#--- all-a64.yaml
--- !COFF
header:
  Machine: IMAGE_FILE_MACHINE_ARM64
  Characteristics: [ ]
sections:
  - Name: .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment: 4
    SectionData: 000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000900000009100000000
    Relocations:
      - { VirtualAddress: 0, SymbolName: fn, Type: IMAGE_REL_ARM64_ADDR32 }
      - { VirtualAddress: 8, SymbolName: fn, Type: IMAGE_REL_ARM64_ADDR32NB }
      - { VirtualAddress: 16, SymbolName: fn, Type: IMAGE_REL_ARM64_BRANCH26 }
      - { VirtualAddress: 24, SymbolName: fn, Type: IMAGE_REL_ARM64_PAGEBASE_REL21 }
      - { VirtualAddress: 32, SymbolName: fn, Type: IMAGE_REL_ARM64_REL21 }
      - { VirtualAddress: 40, SymbolName: fn, Type: IMAGE_REL_ARM64_PAGEOFFSET_12A }
      - { VirtualAddress: 48, SymbolName: fn, Type: IMAGE_REL_ARM64_PAGEOFFSET_12L }
      - { VirtualAddress: 56, SymbolName: fn, Type: IMAGE_REL_ARM64_SECREL }
      - { VirtualAddress: 64, SymbolName: fn, Type: IMAGE_REL_ARM64_SECREL_LOW12A }
      - { VirtualAddress: 72, SymbolName: fn, Type: IMAGE_REL_ARM64_SECREL_HIGH12A }
      - { VirtualAddress: 80, SymbolName: fn, Type: IMAGE_REL_ARM64_SECREL_LOW12L }
      - { VirtualAddress: 88, SymbolName: fn, Type: IMAGE_REL_ARM64_TOKEN }
      - { VirtualAddress: 96, SymbolName: fn, Type: IMAGE_REL_ARM64_SECTION }
      - { VirtualAddress: 104, SymbolName: fn, Type: IMAGE_REL_ARM64_BRANCH19 }
      - { VirtualAddress: 112, SymbolName: fn, Type: IMAGE_REL_ARM64_BRANCH14 }
      - { VirtualAddress: 120, SymbolName: fn, Type: IMAGE_REL_ARM64_REL32 }
      - { VirtualAddress: 128, SymbolName: fn, Type: IMAGE_REL_LLVM_ADDR32NB_CALL }
      - { VirtualAddress: 136, SymbolName: fn, Type: IMAGE_REL_LLVM_REL32_CALL }
      - { VirtualAddress: 144, SymbolName: fn, Type: IMAGE_REL_LLVM_BRANCH_TAIL }
      - { VirtualAddress: 152, SymbolName: fn, Type: IMAGE_REL_LLVM_MOVW_UABS_G0 }
      - { VirtualAddress: 160, SymbolName: fn, Type: IMAGE_REL_LLVM_MOVW_UABS_G0_NC }
      - { VirtualAddress: 168, SymbolName: fn, Type: IMAGE_REL_LLVM_MOVW_UABS_G1 }
      - { VirtualAddress: 176, SymbolName: fn, Type: IMAGE_REL_LLVM_LOGIMM }
      - { VirtualAddress: 184, SymbolName: fn, Type: IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL }
      - { VirtualAddress: 192, SymbolName: fn, Type: IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL }
      - { VirtualAddress: 200, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_CALL }
      - { VirtualAddress: 208, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_JUMP }
      - { VirtualAddress: 216, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_JUMP_TAIL }
      - { VirtualAddress: 224, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_READ }
      - { VirtualAddress: 232, SymbolName: fn, Type: IMAGE_REL_LLVM_FUSED_TEST }
      - { VirtualAddress: 240, SymbolName: fn, Type: IMAGE_REL_LLVM_TPOFF_HIGH12A }
      - { VirtualAddress: 248, SymbolName: fn, Type: IMAGE_REL_LLVM_TPOFF_LOW12A }
      - { VirtualAddress: 256, SymbolName: fn, Type: IMAGE_REL_LLVM_TPOFF_LOW12L }
      - { VirtualAddress: 264, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN8 }
      - { VirtualAddress: 264, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 272, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN16 }
      - { VirtualAddress: 272, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 280, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN32 }
      - { VirtualAddress: 280, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 288, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN64 }
      - { VirtualAddress: 288, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 296, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN_ULEB }
      - { VirtualAddress: 296, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 304, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN_SLEB }
      - { VirtualAddress: 304, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 312, SymbolName: fn, Type: IMAGE_REL_LLVM_SSPAN_WORDS18 }
      - { VirtualAddress: 312, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 320, SymbolName: fn, Type: IMAGE_REL_LLVM_USPAN8 }
      - { VirtualAddress: 320, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 328, SymbolName: fn, Type: IMAGE_REL_LLVM_USPAN16 }
      - { VirtualAddress: 328, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 336, SymbolName: fn, Type: IMAGE_REL_LLVM_USPAN8_W }
      - { VirtualAddress: 336, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 344, SymbolName: fn, Type: IMAGE_REL_LLVM_USPAN16_W }
      - { VirtualAddress: 344, SymbolName: main, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 352, SymbolName: main, Type: IMAGE_REL_ARM64_PAGEOFFSET_12A }
      - { VirtualAddress: 352, SymbolName: fn, Type: IMAGE_REL_ARM64_PAIR }
      - { VirtualAddress: 360, SymbolName: fn, Type: IMAGE_REL_ARM64_PAGEBASE_REL21 }
      - { VirtualAddress: 364, SymbolName: fn, Type: IMAGE_REL_ARM64_PAGEOFFSET_12A }
  - Name: .data
    Characteristics: [ IMAGE_SCN_CNT_INITIALIZED_DATA, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE ]
    Alignment: 8
    SectionData: '00000000000000000000000000000000000000000000000000000000000000000000000000000000'
    Relocations:
      - { VirtualAddress: 0, SymbolName: fn, Type: IMAGE_REL_ARM64_ADDR64 }
      - { VirtualAddress: 8, SymbolName: fn, Type: IMAGE_REL_ARM64_ADDR32 }
      - { VirtualAddress: 12, SymbolName: fn, Type: IMAGE_REL_ARM64_ADDR32NB }
      - { VirtualAddress: 16, SymbolName: fn, Type: IMAGE_REL_ARM64_REL32 }
      - { VirtualAddress: 20, SymbolName: fn, Type: IMAGE_REL_ARM64_SECREL }
      - { VirtualAddress: 24, SymbolName: fn, Type: IMAGE_REL_LLVM_ADDR32NB_CALL }
      - { VirtualAddress: 28, SymbolName: fn, Type: IMAGE_REL_LLVM_REL32_CALL }
  - Name: .llvm_link_records
    Characteristics: [ IMAGE_SCN_LNK_INFO ]
    Alignment: 1
    SectionData: 4C4C524301000100
symbols:
  - { Name: '@comp.id', Value: 0x4C4C0000, SectionNumber: -1, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_NULL, StorageClass: IMAGE_SYM_CLASS_STATIC }
  - { Name: main, Value: 0, SectionNumber: 1, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_FUNCTION, StorageClass: IMAGE_SYM_CLASS_EXTERNAL }
  - { Name: fn, Value: 0, SectionNumber: 0, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_FUNCTION, StorageClass: IMAGE_SYM_CLASS_EXTERNAL }
...
