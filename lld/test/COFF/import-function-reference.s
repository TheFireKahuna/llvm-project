# REQUIRES: x86, aarch64

## A reference to an imported function by its own name has a value only from a
## provider: the thunk for calls and jumps, the import's entry for an address
## that code takes, the loader for a word of data; any other is one error.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{link} = lld-link -dll -noentry -include:main -errorlimit:0 \
# DEFINE:   -call-graph-profile-sort:none
# DEFINE: %{x64} = llvm-mc -filetype=obj -triple=x86_64-windows-itanium
# DEFINE: %{a64} = llvm-mc -filetype=obj -triple=aarch64-windows-itanium
# RUN: lld-link -machine:x64 -def:imp.def -out:imp.lib
# RUN: lld-link -machine:arm64 -def:imp.def -out:imp-a64.lib

## Every relocation type once against fn, in code and in data: all but the
## provided ones, which --implicit-check-not names, are one error each.
# RUN: yaml2obj all.yaml -o all.obj
# RUN: yaml2obj all-a64.yaml -o all-a64.obj
# RUN: not %{link} imp.lib all.obj -out:all.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=ALL --implicit-check-not="needs an address" \
# RUN:   --implicit-check-not='{{_(ADDR64|ADDR32NB_CALL|REL32_CALL|BRANCH_TAIL|BRANCH32|JUMP32|JUMP8|LEA|LEA_OFFSET) needs}}'
# RUN: not %{link} imp-a64.lib all-a64.obj -out:all.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=ALL-A64 --implicit-check-not="needs an address" \
# RUN:   --implicit-check-not='{{_(ADDR64|ADDR32NB_CALL|REL32_CALL|BRANCH_TAIL|BRANCH26|BRANCH19|BRANCH14|FUSED_CALL|FUSED_JUMP|FUSED_JUMP_TAIL) needs|text\+0x16[8c]:}}'

# ALL: error: all.obj: .text+0x3: IMAGE_REL_AMD64_ADDR32 needs an address for fn, which imp.dll provides and this image has none of; call it, or use __imp_fn, in assembly fn@GOTPCREL(%rip)
# ALL-COUNT-40: needs an address for fn
# ALL-A64: error: all-a64.obj: .text+0x0: IMAGE_REL_ARM64_ADDR32 needs an address for fn, which imp.dll provides and this image has none of; call it, or use __imp_fn, in assembly :got:fn
# ALL-A64-COUNT-39: needs an address for fn

## ELF-style assembly: calls and jumps reach the thunk, any address-take is the
## import's, and a call-only field holds the thunk.
# RUN: %{x64} good.s -o good.obj
# RUN: %{link} imp.lib good.obj -out:good.dll
# RUN: llvm-objdump -d -s -j .text -j .data good.dll | \
# RUN:   FileCheck --check-prefix=GOOD %s
# RUN: llvm-readobj --coff-basereloc good.dll | FileCheck --check-prefix=NORELOC %s

## The word is the loader's, with the import's name until it is bound.
# GOOD:      Contents of section .data:
# GOOD-NEXT: 180003000 {{[0-9a-f]*[1-9a-f][0-9a-f]*}} 00000000 30100000 24e0ffff
# GOOD:      180001000: e8 {{.*}} callq 0x180001030
# GOOD-NEXT: e8 {{.*}} callq 0x180001030
# GOOD-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax {{.*}}# 0x[[#%x,IAT:]]
# GOOD-NEXT: 48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#IAT]]
# GOOD-NEXT: jne 0x180001030
# GOOD-NEXT: jmp 0x180001030
# GOOD:      180001030: ff 25 {{.*}} jmpq *{{.*}}(%rip) {{.*}}# 0x[[#IAT]]
# NORELOC:      BaseReloc [
# NORELOC-NEXT: ]

# RUN: %{a64} good-a64.s -o good-a64.obj
# RUN: %{link} imp-a64.lib good-a64.obj -out:good-a64.dll
# RUN: llvm-objdump -d -s -j .text -j .data good-a64.dll | \
# RUN:   FileCheck --check-prefix=GOOD-A64 %s
# RUN: llvm-readobj --coff-basereloc good-a64.dll | FileCheck --check-prefix=NORELOC %s

# GOOD-A64:      Contents of section .data:
# GOOD-A64-NEXT: 180003000 {{[0-9a-f]*[1-9a-f][0-9a-f]*}} 00000000 18100000 0ce0ffff
# GOOD-A64:      180001000: {{.*}} bl 0x180001018
# GOOD-A64-NEXT: ldr x0, 0x[[#%x,IAT:]]
# GOOD-A64-NEXT: ldr x1, 0x[[#IAT]]
# GOOD-A64-NEXT: cbz x0, 0x180001018
# GOOD-A64-NEXT: tbz w0, #0x0, 0x180001018
# GOOD-A64-NEXT: b 0x180001018
# GOOD-A64-NEXT: 180001018: {{.*}} ldr x16, 0x[[#IAT]]
# GOOD-A64-NEXT: br x16

## Errors: reads of or calls through the memory at the function, a branch past
## it, an address no field can load, an add not proven to follow its adrp.
# RUN: %{x64} bad.s -o bad.obj
# RUN: not %{link} imp.lib bad.obj -out:bad.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=BAD --implicit-check-not="needs an address"
# BAD:      .text+0x3: IMAGE_REL_LLVM_MEM_LOAD needs an address for ext
# BAD-NEXT: .text+0x9: IMAGE_REL_LLVM_MEM_CALL needs an address for ext
# BAD-NEXT: .text+0xf: IMAGE_REL_LLVM_MEM_JUMP_TAIL needs an address for ext
# BAD-NEXT: .text+0x14: IMAGE_REL_LLVM_BRANCH32 needs an address for ext
# BAD-NEXT: .text+0x1a: IMAGE_REL_LLVM_REL32_INSN needs an address for ext
# BAD-NEXT: .text+0x21: IMAGE_REL_LLVM_LEA_NARROW needs an address for ext
# BAD-NEXT: .text+0x2c: IMAGE_REL_LLVM_LEA_NARROW needs an address for ext
# BAD-NEXT: .data+0x0: IMAGE_REL_AMD64_ADDR32NB needs an address for ext
# BAD-NEXT: .data+0x4: IMAGE_REL_AMD64_REL32 needs an address for ext

# RUN: %{a64} bad-a64.s -o bad-a64.obj
# RUN: not %{link} imp-a64.lib bad-a64.obj -out:bad.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=BAD-A64 --implicit-check-not="needs an address"
# BAD-A64:      .text+0x0: IMAGE_REL_ARM64_PAGEBASE_REL21 needs an address for ext
# BAD-A64-NEXT: .text+0x4: IMAGE_REL_ARM64_PAGEOFFSET_12L needs an address for ext
# BAD-A64-NEXT: .text+0x8: IMAGE_REL_ARM64_REL21 needs an address for ext
# BAD-A64-NEXT: .text+0xc: IMAGE_REL_ARM64_PAGEBASE_REL21 needs an address for ext
# BAD-A64-NEXT: .text+0x14: IMAGE_REL_ARM64_PAGEOFFSET_12A needs an address for ext
# BAD-A64-NEXT: .text+0x18: IMAGE_REL_ARM64_PAGEBASE_REL21 needs an address for ext
# BAD-A64-NEXT: .text+0x1c: IMAGE_REL_ARM64_PAGEOFFSET_12A needs an address for ext

## A fused read that stays a load reads what is at the function.
# RUN: %{a64} fused-a64.s -o fused-a64.obj
# RUN: not %{link} imp-a64.lib fused-a64.obj -out:bad.dll 2>&1 | FileCheck %s \
# RUN:   --check-prefix=FUSED-A64 --implicit-check-not="needs an address"
# FUSED-A64:      .text+0x0: IMAGE_REL_ARM64_PAGEBASE_REL21 needs an address for ext
# FUSED-A64-NEXT: .text+0x4: IMAGE_REL_LLVM_FUSED_READ needs an address for ext

## Delay-loaded imports, foreign objects (but for a field before an immediate)
## and unloaded sections keep the thunk.
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
# FOREIGN: error: foreign.obj: .text+0x1a: IMAGE_REL_AMD64_REL32_1 needs an address for ext

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
      - { VirtualAddress: 248, SymbolName: fn, Type: IMAGE_REL_LLVM_TPOFF_LOW12A_NC }
      - { VirtualAddress: 256, SymbolName: fn, Type: IMAGE_REL_LLVM_TPOFF_LOW12L_NC }
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
