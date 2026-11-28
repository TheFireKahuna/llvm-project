# REQUIRES: x86, aarch64

## Resizable-section sites that do not match their type are reported: bad
## padding, an outside SREL32, non-jump BRANCH_TAIL/JUMP32/JUMP8, overlapping
## rewrites, a rewrite in a type prefix, relocations out of offset order.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix tls.s -o tls.obj
# RUN: obj2yaml x64.obj | sed -e 's/IMAGE_REL_LLVM_BRANCH32/IMAGE_REL_LLVM_BRANCH_TAIL/' \
# RUN:   -e 's/IMAGE_REL_LLVM_LEA/IMAGE_REL_LLVM_JUMP32/' \
# RUN:   -e 's/IMAGE_REL_LLVM_MEM_LOAD/IMAGE_REL_LLVM_JUMP8/' | yaml2obj -o x64-call.obj
# RUN: obj2yaml a64.obj | sed -e 's/IMAGE_REL_ARM64_BRANCH26/IMAGE_REL_LLVM_BRANCH_TAIL/' \
# RUN:   -e 's/1F2003D5/00000000/g' | yaml2obj -o a64-call.obj

# RUN: obj2yaml x64.obj | sed 's/VirtualAddress: .*/VirtualAddress: 4096/' | \
# RUN:   yaml2obj -o x64-beyond.obj
# RUN: obj2yaml a64.obj | sed 's/VirtualAddress: .*/VirtualAddress: 4096/' | \
# RUN:   yaml2obj -o a64-beyond.obj

# RUN: not lld-link -dll -noentry -export:a x64-call.obj -out:x64.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=X64 %s
# RUN: not lld-link -machine:arm64 -dll -noentry -export:a a64-call.obj -out:a64.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=A64 %s
# RUN: not lld-link -entry:a -subsystem:console -export:b -export:c tls.obj -out:tls.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=TLS %s
# RUN: not lld-link -dll -noentry -export:a x64-beyond.obj -out:x64.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=BEYOND %s
# RUN: not lld-link -machine:arm64 -dll -noentry -export:a a64-beyond.obj \
# RUN:   -out:a64.dll 2>&1 | FileCheck --check-prefix=BEYOND %s

# X64:      error: x64-call.obj: .text$a+0x5: IMAGE_REL_LLVM_ALIGN
# X64-NEXT: error: x64-call.obj: .text$a+0x10: IMAGE_REL_LLVM_ALIGN
# X64-NEXT: error: x64-call.obj: .text$a+0x10: IMAGE_REL_LLVM_ALIGN
# X64-NEXT: error: x64-call.obj: .text$a+0x10: IMAGE_REL_AMD64_SREL32
# X64-NEXT: error: x64-call.obj: .text$a+0x15: IMAGE_REL_LLVM_BRANCH_TAIL
# X64-NEXT: error: x64-call.obj: .text$a+0x1c: IMAGE_REL_LLVM_JUMP32
# X64-NEXT: error: x64-call.obj: .text$a+0x23: IMAGE_REL_LLVM_JUMP8
# X64-NOT:  error:

# A64:      error: a64-call.obj: .text$a+0x4: IMAGE_REL_LLVM_ALIGN
# A64-NEXT: error: a64-call.obj: .text$a+0x10: IMAGE_REL_LLVM_BRANCH_TAIL
# A64-NOT:  error:

# TLS:      error: tls.obj: .text$a: the rewrites at 0x0 and 0x7 overlap
# TLS-NEXT: error: tls.obj: .text$b+0x4: a rewrite in a function's type prefix
# TLS-NEXT: error: tls.obj: .text$c+0x0: a rewrite in a function's type prefix
# TLS-NOT:  error:

# BEYOND:     error: {{.*}}relocation points beyond the end of
# BEYOND-NOT: does not have the instructions

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium order.s -o order.obj
# RUN: obj2yaml order.obj | sed 's/VirtualAddress:  0$/VirtualAddress:  8/' | \
# RUN:   yaml2obj -o disorder.obj
# RUN: not lld-link -entry:main -subsystem:console disorder.obj -out:order.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=ORDER %s

# ORDER: error: disorder.obj: the relocations of .text, which the link may resize, are not in order

#--- x64.s
  .section .text$a,"xr",discard,a
  .globl a
  .p2align 4
a:
.La:
  jmp b
  .reloc ., IMAGE_REL_LLVM_ALIGN, 0x1e4
  .fill 11, 1, 0xcc
  .reloc ., IMAGE_REL_LLVM_ALIGN, 0x1e6
  .reloc ., IMAGE_REL_LLVM_ALIGN, 0x80001e0
  .reloc ., IMAGE_REL_AMD64_SREL32, b
  .long 0
  callq b
  leaq b(%rip), %rax
  movq b(%rip), %rax
.Lend:
  movq __imp_b@FUSED(%rip), %rcx
  .section .rdata,"dr"
  .long .Lend - .La
  .section .text$b,"xr",discard,b
  .globl b
b:
  retq

#--- a64.s
  .section .text$a,"xr",discard,a
  .globl a
  .p2align 4
a:
  b c
  .p2align 4
  bl c
  adrp x0, c
  add x0, x0, :lo12:c
  .section .text$c,"xr",discard,c
  .globl c
c:
  ret

#--- tls.s
.macro site
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  movq __imp___tp_register$init(%rip), %rdx
  movq (%r14,%rdx), %rdx
  rorq $17, %rdx
  xorq __imp___tp_register$init+16(%rip), %rdx
.endm
  .section .text$a,"xr",discard,a
  .globl a
a:
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  movq __imp___tp_register$init(%rip), %rdx
  .reloc ., IMAGE_REL_LLVM_ALIGN, 0
  movq (%r14,%rdx), %rdx
  rorq $17, %rdx
  xorq __imp___tp_register$init+16(%rip), %rdx
  jmp a
  .section .text$b,"xr",discard,b
__cfi_b:
  .long 0
  site
  .globl b
b:
  jmp a
  .section .text$c,"xr",discard,c
__cfi_c:
  callq *__imp_a(%rip)
  .globl c
c:
  jmp a
  movq __imp_a@FUSED(%rip), %rcx
  .section .tls$,"dw"
  .long 0

#--- order.s
.globl main, f
main:
  callq *__imp_f(%rip)
  .reloc main, IMAGE_REL_AMD64_ABSOLUTE
f:
  ret
  movq __imp_f@FUSED(%rip), %rcx
