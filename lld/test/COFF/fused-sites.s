# REQUIRES: x86, aarch64

## Fused calls, jumps and reads bind by their pointer: a local import's to bl
## or b, a delay-loaded one's to its thunk, any other's stay; a dead read is
## deleted where the section resizes, else nopped or turned into overrides.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab \
# DEFINE:   -call-graph-profile-sort:none

# RUN: %{mc} -triple=x86_64-windows-itanium -mattr=+egpr x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium defs.s -o defs-x64.obj
# RUN: lld-link -dll -noentry -export:f defs-x64.obj -out:f-x64.dll \
# RUN:   -implib:f-x64.lib
# RUN: %{link} -out:x64.exe x64.obj defs-x64.obj 2>&1 | count 0
# RUN: llvm-objdump -d x64.exe | FileCheck --check-prefix=X64 %s
# RUN: llvm-readobj --sections x64.exe | FileCheck --check-prefix=NOPTR %s

# X64-LABEL: <main>:
# X64-NEXT:  31 c0 xorl %eax, %eax
# X64-NEXT:  31 c9 xorl %ecx, %ecx
# X64-NEXT:  retq

## In a fixed section a dead read is overrides on the next instruction, or one
## nop beside another dead read, past 15 bytes, or across a 32-byte boundary.
# X64-LABEL: <same>:
# X64-NEXT:  2e 2e 2e 2e 2e 2e 2e 29 c0 subl %eax, %eax
# X64-NEXT:  0f 1f 80 00 00 00 00 nopl
# X64-NEXT:  0f 1f 80 00 00 00 00 nopl
# X64-NEXT:  31 d2 xorl %edx, %edx
# X64-NEXT:  0f 1f 84 00 00 00 00 00 nopl
# X64-NEXT:  movabsq
# X64-NEXT:  2e 2e 2e 2e 2e 2e 2e 2e 31 d2 xorl %edx, %edx
# X64-NEXT:  0f 1f 80 00 00 00 00 nopl
# X64-NEXT:  : 90 nop
# X64:       7b: 90 nop
# X64-NEXT:  7c: 0f 1f 80 00 00 00 00 nopl
# X64-NEXT:  83: e8 {{.*}} callq {{.*}} <f>

## No pointer is left: the image's read-only data is its empty offer table.
# NOPTR:      Name: .rdata
# NOPTR-NEXT: VirtualSize: 0x18

## Where the assembler did not check the sites, a fused read is the load it is,
## reaching the pointer's target, and the pointer goes.
# RUN: obj2yaml x64.obj | \
# RUN:   sed 's/SectionData:     4C4C52430108/SectionData:     4C4C52430100/' | \
# RUN:   yaml2obj -o x64-unchecked.obj
# RUN: %{link} -out:x64-unchecked.exe x64-unchecked.obj defs-x64.obj 2>&1 | \
# RUN:   count 0
# RUN: llvm-objdump -d x64-unchecked.exe | FileCheck --check-prefix=UNCHECKED %s
# RUN: llvm-readobj --sections x64-unchecked.exe | \
# RUN:   FileCheck --check-prefix=NOPTR %s

# UNCHECKED-LABEL: <main>:
# UNCHECKED-NEXT:  48 8d 35 {{.*}} leaq {{.*}} <f>
# UNCHECKED-NEXT:  xorl %eax, %eax
# UNCHECKED-NEXT:  4c 8d 05 {{.*}} leaq {{.*}} <f>
# UNCHECKED-NEXT:  d5 48 8d 05 {{.*}} leaq {{.*}} <f>
# UNCHECKED-NEXT:  xorl %ecx, %ecx
# UNCHECKED-NEXT:  retq

## A .refptr cell that the link binds is such a pointer.
# RUN: %{mc} -triple=x86_64-windows-itanium refptr.s -o refptr.obj
# RUN: %{link} -out:refptr.exe refptr.obj defs-x64.obj 2>&1 | count 0
# RUN: llvm-objdump -d refptr.exe | FileCheck --check-prefix=REFPTR %s
# RUN: llvm-readobj --sections refptr.exe | FileCheck --check-prefix=NOPTR %s

# REFPTR-LABEL: <main>:
# REFPTR-NEXT:  xorl %eax, %eax
# REFPTR-NEXT:  : e8 {{.*}} callq {{.*}} <f>
# REFPTR-NEXT:  retq

## Bytes that do not match a fused type are an error.
# RUN: %{mc} -triple=x86_64-windows-itanium bad.s -o bad.obj
# RUN: %{mc} -triple=x86_64-windows-itanium bytes.s -o bytes.obj
# RUN: llvm-objcopy --dump-section=.text=bytes.bin bytes.obj
# RUN: llvm-objcopy --update-section=.text=bytes.bin bad.obj
# RUN: not %{link} -out:bad.exe bad.obj defs-x64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD --implicit-check-not=error: %s

# BAD:      error: bad.obj: .text+0x3: IMAGE_REL_LLVM_FUSED_READ
# BAD-NEXT: error: bad.obj: .text+0xa: IMAGE_REL_LLVM_FUSED_READ

## A fixed section's dead read is disposed of through the relocations beside
## it, which must be in offset order.
# RUN: %{mc} -triple=x86_64-windows-itanium order.s -o order.obj
# RUN: obj2yaml order.obj | sed 's/VirtualAddress:  0$/VirtualAddress:  8/' | \
# RUN:   yaml2obj -o disorder.obj
# RUN: not %{link} -out:order.exe disorder.obj defs-x64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=ORDER %s

# ORDER: error: disorder.obj: .text+0x3: IMAGE_REL_LLVM_FUSED_READ

# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium defs.s -o defs-a64.obj
# RUN: lld-link -dll -noentry -export:f defs-a64.obj -out:f-a64.dll \
# RUN:   -implib:f-a64.lib
# RUN: %{link} -out:a64.exe a64.obj defs-a64.obj 2>&1 | count 0
# RUN: llvm-objdump -d a64.exe | FileCheck --check-prefix=A64 %s
# RUN: llvm-readobj --sections a64.exe | FileCheck --check-prefix=NOPTR %s

## An adrp the read does not write stays, naming f's page; main's final jump,
## which no unwind frame covers, is deleted, f following it.
# A64-LABEL: <main>:
# A64-NEXT:  bl {{.*}} <f>
# A64-NEXT:  adrp x9, {{.*}} <main>
# A64-NEXT:  bl {{.*}} <f>
# A64-EMPTY:
# A64-NEXT:  <f>:
# A64-LABEL: <same>:
# A64-NEXT:  nop
# A64-NEXT:  nop
# A64-NEXT:  bl {{.*}} <f>
# A64-NEXT:  adrp x9, {{.*}} <main>
# A64-NEXT:  nop
# A64-NEXT:  bl {{.*}} <f>
# A64-NEXT:  nop
# A64-NEXT:  nop
# A64-NEXT:  b {{.*}} <f>

# RUN: %{link} -out:a64-imp.exe a64.obj f-a64.lib 2>&1 | count 0
# RUN: llvm-objdump -d a64-imp.exe | FileCheck --check-prefix=A64-IMP %s

## A read that stays is the load it is: with its adrp, one literal load
## where the link resizes the section.
# A64-IMP-LABEL: <main>:
# A64-IMP-NEXT:  ldr x8, 0x[[#%x,]] <__imp_f>
# A64-IMP-NEXT:  blr x8
# A64-IMP-NEXT:  adrp x9,
# A64-IMP-NEXT:  ldr x10, [x9]
# A64-IMP-NEXT:  blr x10
# A64-IMP-NEXT:  ldr x16, 0x[[#%x,]] <__imp_f>
# A64-IMP-NEXT:  br x16
# A64-IMP-LABEL: <same>:
# A64-IMP-NEXT:  adrp x8,
# A64-IMP-NEXT:  ldr x8, [x8]

# RUN: %{mc} -triple=aarch64-windows-itanium helper.s -o helper-a64.obj
## Nothing else refers to the thunk, which dead stripping would leave out.
# RUN: %{link} -opt:ref -out:a64-delay.exe a64.obj helper-a64.obj f-a64.lib \
# RUN:   -delayload:f-a64.dll 2>&1 | count 0
# RUN: llvm-objdump -d a64-delay.exe | FileCheck --check-prefix=A64-DELAY %s

# A64-DELAY-LABEL: <main>:
# A64-DELAY-NEXT:  bl 0x[[#%x,THUNK:]]
# A64-DELAY-NEXT:  adrp x9,
# A64-DELAY-NEXT:  bl 0x[[#THUNK]]
# A64-DELAY-NEXT:  b 0x[[#THUNK]]
# A64-DELAY-LABEL: <same>:
# A64-DELAY-NEXT:  nop
# A64-DELAY-NEXT:  nop
# A64-DELAY-NEXT:  bl 0x[[#THUNK]]
# A64-DELAY:       [[#THUNK]]: {{.*}} ldr x16, 0x[[#%x,]]
# A64-DELAY-NEXT:  br x16

## A bl the link makes takes a range extension thunk where it does not reach.
# RUN: %{mc} -triple=aarch64-windows-itanium far.s -o far.obj
# RUN: %{link} -out:far.exe far.obj 2>&1 | count 0
# RUN: llvm-objdump -d far.exe | FileCheck --check-prefix=FAR %s

# FAR-LABEL: <main>:
# FAR-NEXT:  bl 0x[[#%x,THUNK:]]
# FAR-NEXT:  ret
# FAR-NEXT:  [[#THUNK]]: {{.*}} adrp x16, {{.*}} <far>
# FAR-NEXT:  add x16, x16, #0x0
# FAR-NEXT:  br x16

# RUN: %{mc} -triple=aarch64-windows-itanium bad-a64.s -o bad-a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium bytes-a64.s -o bytes-a64.obj
# RUN: llvm-objcopy --dump-section=.text=bytes-a64.bin bytes-a64.obj
# RUN: llvm-objcopy --update-section=.text=bytes-a64.bin bad-a64.obj
# RUN: not %{link} -out:bad-a64.exe bad-a64.obj defs-a64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD-A64 --implicit-check-not=error: %s

# BAD-A64:      error: bad-a64.obj: .text+0x8: IMAGE_REL_LLVM_FUSED_CALL
# BAD-A64-NEXT: error: bad-a64.obj: .text+0x14: IMAGE_REL_LLVM_FUSED_JUMP_TAIL
# BAD-A64-NEXT: error: bad-a64.obj: .text+0x4: IMAGE_REL_LLVM_FUSED_READ

#--- x64.s
.globl main
main:
  movq __imp_f@FUSED(%rip), %rsi
  xorl %eax, %eax
  movq __imp_f@FUSED(%rip), %r8
  movq __imp_f@FUSED(%rip), %r16
  xorl %ecx, %ecx
  retq

## The fill is data, so the link does not resize this section.
  .section .text$d,"xr"
  .globl same
## The sub leaves the flags dead at the read, which the read's field says in
## a bit beside the length of the sub.
same:
  movq __imp_f@FUSED(%rip), %rsi
  subl %eax, %eax
  movq __imp_f@FUSED(%rip), %rax
  movq __imp_f@FUSED(%rip), %rcx
  xorl %edx, %edx
  movq __imp_f@FUSED(%rip), %r16
  movabsq $0x1122334455667788, %rdx
  movq __imp_f@FUSED(%rip), %r17
  xorl %edx, %edx
  movq __imp_f@FUSED(%rip), %rax
  .byte 0x90
## The read ends three bytes past a 32-byte boundary, before a call.
  .p2align 5, 0xcc
  .skip 28, 0x90
  movq __imp_f@FUSED(%rip), %rsi
  callq f
  .p2align 2, 0

#--- a64.s
.macro sites
  adrp x8, __imp_f
  ldr x8, [x8, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x8
  adrp x9, __imp_f
  ldr x10, [x9, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x10
  adrp x16, __imp_f
  ldr x16, [x16, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  br x16
.endm
.globl main
main:
  sites

## The fill is data, so the link does not resize this section.
  .section .text$d,"xr"
  .globl same
same:
  sites
  .p2align 3, 0

#--- defs.s
.globl f
f:
  ret

#--- helper.s
.globl __delayLoadHelper2
__delayLoadHelper2:
  ret

#--- refptr.s
.globl main
main:
  xorl %eax, %eax
  movq .refptr.f@LOAD_CALL(%rip), %rsi
  callq *%rsi
  ret
  .section .rdata$.refptr.f,"dr",discard,.refptr.f
  .globl .refptr.f
.refptr.f:
  .quad f

#--- bad.s
.globl main
main:
  movq __imp_f@FUSED(%rip), %rsi
  movq __imp_f@FUSED(%rip), %rax
  retq

## The same lengths: lea, and a load whose field is more than twice an
## instruction's length.
#--- bytes.s
  .byte 0x48, 0x8d, 0x35, 0, 0, 0, 0
  .byte 0x48, 0x8b, 0x05, 0x40, 0, 0, 0
  .byte 0xc3

#--- order.s
.globl main
main:
  movq __imp_f@FUSED(%rip), %rsi
  xorl %eax, %eax
  .reloc main, IMAGE_REL_AMD64_ABSOLUTE
  ret
  .p2align 2, 0

#--- far.s
.globl main
main:
  adrp x8, __imp_far
  ldr x8, [x8, :fused_lo12:__imp_far]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_far
  blr x8
  ret
  .section .far,"bw"
  .zero 0x8000000
  .globl far
far:
  .zero 4

#--- bad-a64.s
.globl main
main:
  adrp x8, __imp_f
  ldr x8, [x8, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x8
  adrp x16, __imp_f
  ldr x16, [x16, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  br x16

## A 32-bit load, a br for the call and a blr for the jump.
#--- bytes-a64.s
  adrp x8, main
  ldr w8, [x8]
  br x8
  adrp x16, main
  ldr x16, [x16]
  blr x16
