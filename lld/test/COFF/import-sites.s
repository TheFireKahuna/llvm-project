# REQUIRES: x86, aarch64

## Typed loads, calls, jumps and load-call pairs through a pointer the image
## defines reach the symbol in the same length (their own where code moves);
## jcc over jmp becomes one jcc; ARM64 adrp+ldr becomes adr or movz.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab \
# DEFINE:   -call-graph-profile-sort:none

# RUN: %{mc} -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium defs.s -o defs-x64.obj
# RUN: %{link} -out:x64.exe x64.obj defs-x64.obj
# RUN: llvm-objdump -d x64.exe | FileCheck --check-prefix=X64 %s
# RUN: llvm-readobj --sections x64.exe | FileCheck --check-prefix=NOPTR %s

# X64-LABEL: <main>:
# X64-NEXT:  48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax {{.*}} <d>
# X64-NEXT:  d5 48 8d 25 {{.*}} leaq {{.*}}(%rip), %r20 {{.*}} <d>
# X64-NEXT:  : e8 {{.*}} callq {{.*}} <f>
# X64-NEXT:  48 c7 c1 00 00 00 00 movq $0x0, %rcx
# X64-NEXT:  45 31 db xorl %r11d, %r11d
# X64-NEXT:  41 ff d3 callq *%r11
# X64-NEXT:  48 c7 c2 34 12 00 00 movq $0x1234, %rdx
# X64-NEXT:  48 8b 35 {{.*}} movq {{.*}}(%rip), %rsi
# X64-NEXT:  : eb {{..}} jmp {{.*}} <f>
# X64-NEXT:  : eb {{..}} jmp {{.*}} <f>
# X64-EMPTY:
# X64-NEXT:  <framed>:
# X64-NEXT:  subq
# X64-NEXT:  : eb {{..}} jmp {{.*}} <f>
## A jump into its own section keeps its pointer (a direct one would end no
## epilogue for the unwinder); a call does not.
# X64-LABEL: <self>:
# X64-NEXT:  : e8 {{.*}} callq {{.*}} <self>
# X64-NEXT:  ff 25 {{.*}} jmpq *{{.*}}(%rip) {{.*}} <__imp_self>
# X64-LABEL: <pair>:
# X64-NEXT:  : e8 {{.*}} callq {{.*}} <f>
# X64-NEXT:  49 c7 c3 00 00 00 00 movq $0x0, %r11
# X64-NEXT:  41 ff d3 callq *%r11
# X64-NEXT:  48 c7 c1 34 12 00 00 movq $0x1234, %rcx
# X64-NEXT:  ff d1 callq *%rcx
# X64-LABEL: <same>:
# X64-NEXT:  67 e8 {{.*}} callq {{.*}} <f>
# X64-NEXT:  : e9 {{.*}} jmp {{.*}} <f>
# X64-NEXT:  : cc int3
# X64-NEXT:  : e9 {{.*}} jmp {{.*}} <f>
# X64-NEXT:  : cc int3
# X64-NEXT:  : cc int3
# X64-NEXT:  : e8 {{.*}} callq {{.*}} <f>
# X64-NEXT:  : 0f 1f 40 00 nopl (%rax)
# X64-NEXT:  : e8 {{.*}} callq {{.*}} <f>
# X64-NEXT:  : 0f 1f 44 00 00 nopl (%rax,%rax)
# X64-NEXT:  : 0f 84 {{.*}} je {{.*}} <f>
# X64-NEXT:  : 66 90 nop
# X64-NEXT:  : 74 06 je
# X64-NEXT:  : ff 25 {{.*}} jmpq *{{.*}}(%rip) {{.*}} <__imp_k>
# X64-LABEL: <far>:
# X64-NEXT:  : e9 80 00 00 00 jmp {{.*}} <g>
## A jcc and the jmp it jumps over become one jcc where the link only
## compacts the code too.
# X64-LABEL: <compacted>:
# X64-NEXT:  : e8 {{.*}} callq {{.*}} <f>
# X64-NEXT:  : 0f 84 {{.*}} je {{.*}} <f>
# X64-NEXT:  : c3 retq

## The Fixup stream records a jump made direct as the jump it is, at its
## field and under the type of its width.
# RUN: %{link} -debug -debugtype:fixup -pdb:x64.pdb -out:x64-fixup.exe \
# RUN:   x64.obj defs-x64.obj
# RUN: llvm-pdbutil dump -fixups x64.pdb | FileCheck --check-prefix=FIXUP %s

# FIXUP:      8022 | 0000  | 00001030 | 0000105C
# FIXUP-NEXT: 8022 | 0000  | 00001032 | 0000105C
# FIXUP-NEXT: 8022 | 0000  | 00001038 | 0000105C
# FIXUP:      8021 | 0000  | 00001099 | 0000111D

## Only d's (offset load), self's and k's (kept jumps) pointers stay; the other
## eight bytes are framed's unwind data, and then comes the empty offer table.
# NOPTR:     Name: .rdata
# NOPTR:     VirtualSize: 0x38{{$}}

# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium defs.s -o defs-a64.obj
# RUN: %{link} -out:a64.exe a64.obj defs-a64.obj -map:a64.map
# RUN: llvm-objdump -d a64.exe > a64.txt
# RUN: cat a64.map >> a64.txt
# RUN: FileCheck --check-prefix=A64 %s < a64.txt

# A64-LABEL: <main>:
# A64-NEXT:  adr x0, 0x[[#%x,D:]]
# A64-NEXT:  mov x1, #0x0
# A64-NEXT:  mov x2, #0x1234
# A64:       {{ }}d [[#%.16x,D]] defs-a64.obj

## Bytes that do not match the type are an error, with a pointer for each.
# RUN: %{mc} -triple=x86_64-windows-itanium bad.s -o bad.obj
# RUN: %{mc} -triple=x86_64-windows-itanium bytes.s -o bytes.obj
# RUN: llvm-objcopy --dump-section=.text=bytes.bin bytes.obj
# RUN: llvm-objcopy --update-section=.text=bytes.bin bad.obj
# RUN: not %{link} -out:bad.exe bad.obj defs-x64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD %s

# BAD:      error: bad.obj: .text+0x3: IMAGE_REL_LLVM_MEM_LOAD
# BAD-NEXT: error: bad.obj: .text+0x9: IMAGE_REL_LLVM_MEM_CALL
# BAD-NEXT: error: bad.obj: .text+0x10: IMAGE_REL_LLVM_MEM_JUMP_P1
# BAD-NEXT: error: bad.obj: .text+0x17: IMAGE_REL_LLVM_MEM_LOAD
# BAD-NEXT: error: bad.obj: .text+0x1d: IMAGE_REL_LLVM_MEM_JUMP_TAIL
# BAD-NEXT: error: bad.obj: .text+0x24: IMAGE_REL_LLVM_MEM_LOAD_CALL
# BAD-NEXT: error: bad.obj: .text+0x2e: IMAGE_REL_LLVM_MEM_JUMP_TAIL

#--- x64.s
.globl main
main:
  movq __imp_d(%rip), %rax
  movq __imp_d(%rip), %r20
  callq *__imp_f(%rip)
  movq __imp_w(%rip), %rcx
  callq *__imp_w(%rip)
  movq __imp_k(%rip), %rdx
  movq __imp_d+8(%rip), %rsi
  jmpq *__imp_f(%rip)
  rex64 jmpq *__imp_f(%rip)
  .seh_proc framed
framed:
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  jmpq *__imp_f(%rip)
  .seh_endproc
  .globl self
self:
  callq *__imp_self(%rip)
  jmpq *__imp_self(%rip)
  .globl pair
pair:
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  movq __imp_w@LOAD_CALL(%rip), %r11
  callq *%r11
  movq __imp_k@LOAD_CALL(%rip), %rcx
  callq *%rcx
  movq __imp_f@FUSED(%rip), %rax
.weak w

## No rewrite removes an instruction of this section, so its code stays.
  .section .text$d,"xr"
  .globl same
same:
  callq *__imp_f(%rip)
  jmpq *__imp_f(%rip)
  rex64 jmpq *__imp_f(%rip)
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  movq __imp_f@LOAD_CALL(%rip), %r10
  callq *%r10
  jne 1f
  jmpq *__imp_f(%rip)
1:
  je 1f
  jmpq *__imp_k(%rip)
1:
  retq
  .p2align 2, 0

## Its target is out of a short jump's reach.
  .section .text$e,"xr"
  .globl far
far:
  jmpq *__imp_g(%rip)
  movq __imp_f@FUSED(%rip), %rax

## A pair makes this section's code compact.
  .section .text$zz,"xr"
  .globl compacted
compacted:
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  jne 1f
  jmpq *__imp_f(%rip)
1:
  retq

#--- a64.s
.globl main
main:
  adrp x0, __imp_d
  ldr x0, [x0, :lo12:__imp_d]
  adrp x1, __imp_w
  ldr x1, [x1, :lo12:__imp_w]
  adrp x2, __imp_k
  ldr x2, [x2, :lo12:__imp_k]
  ret
.weak w

#--- defs.s
.globl f
f:
  ret
  .section .text$y,"xr"
  .fill 128
  .section .text$z,"xr"
.globl g
g:
  ret
.data
.globl d
d:
  .long 7
.globl k
k = 0x1234

#--- bad.s
.globl main
main:
  movq __imp_d(%rip), %rax
  callq *__imp_f(%rip)
  rex64 jmpq *__imp_f(%rip)
  movq __imp_d(%rip), %rax
  jmpq *__imp_f(%rip)
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  jne 1f
  jmpq *__imp_f(%rip)
1:
  retq

## Same lengths: lea, jmp for call, nop for prefix, an offset load, call for
## jump, a call through another register, a jcc-over jmp after two nops.
#--- bytes.s
  .byte 0x48, 0x8d, 0x05, 0, 0, 0, 0
  .byte 0xff, 0x25, 0, 0, 0, 0
  .byte 0x90, 0xff, 0x25, 0, 0, 0, 0
  .byte 0x48, 0x8b, 0x05, 8, 0, 0, 0
  .byte 0xff, 0x15, 0, 0, 0, 0
  .byte 0x48, 0x8b, 0x05, 0, 0, 0, 0, 0xff, 0xd1
  .byte 0x90, 0x90, 0xff, 0x25, 1, 0, 0, 0, 0xc3
