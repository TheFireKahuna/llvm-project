# REQUIRES: x86, aarch64

## Resizing recomputes alignment and boundary padding, SREL32 fields, spans,
## addends, base relocations, guard entries, aliases and override bodies; a
## section with a value that would not fit keeps its size.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix -I %S/Inputs x64.s \
# RUN:   -o x64.obj --x86-align-branch-boundary=32 --x86-align-branch=jcc
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix a64.s -o a64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix tls.s -o tls.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix tls.s -o tls-a64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix \
# RUN:   %S/Inputs/loadconfig-cfg-x64.s -o loadconfig.obj

# RUN: lld-link -dll -noentry -export:f -export:k -export:b -export:X \
# RUN:   -export:mid -guard:cf \
# RUN:   -funcoverride x64.obj tls.obj loadconfig.obj -out:x64.dll
# RUN: llvm-objdump -d x64.dll | FileCheck --check-prefix=X64 %s
# RUN: lld-link -dll -noentry -export:f -debug -debugtype:fixup -pdb:x64.pdb \
# RUN:   x64.obj tls.obj -out:x64-fixup.dll
# RUN: llvm-pdbutil dump -fixups x64.pdb | FileCheck --check-prefix=FIXUP %s
# RUN: llvm-objdump -s -j .rdata -j .data x64.dll | \
# RUN:   FileCheck --check-prefix=X64-DATA %s
# RUN: llvm-readobj --coff-basereloc --coff-load-config x64.dll | \
# RUN:   FileCheck --check-prefix=X64-TABLES %s
# RUN: lld-link -entry:f -subsystem:console -export:mid -guard:cf x64.obj \
# RUN:   tls.obj loadconfig.obj -out:x64.exe
# RUN: llvm-readobj --coff-load-config x64.exe | FileCheck --check-prefix=X64-SET %s
# RUN: lld-link -machine:arm64 -dll -noentry -export:f a64.obj tls-a64.obj \
# RUN:   -out:a64.dll
# RUN: llvm-objdump -d --no-show-raw-insn a64.dll | FileCheck --check-prefix=A64 %s
# RUN: llvm-objdump -s -j .rdata a64.dll | FileCheck --check-prefix=A64-DATA %s

## f: the imul goes and the padding grows by 7, so .Lloop, mid and alias stay;
## .Lpad and pad, at the ALIGN's offset, lie after it.
# X64:      <f>:
# X64-NEXT: 180001000: 4c 03 35 {{.*}} addq
# X64-NEXT: 180001007: 85 c9 testl %ecx, %ecx
# X64-NEXT: 180001009: 74 15 je 0x180001020
# X64-NEXT: 18000100b: 48 8d 05 0e 00 00 00 leaq 0xe(%rip), %rax
# X64-NEXT: 180001012: 48 ba 20 10 00 80 01 00 00 00 movabsq $0x180001020, %rdx
# X64-NEXT: 18000101c: 0f 1f 40 00 nopl (%rax)
# X64:      180001020: ff c9 decl %ecx
# X64-NEXT: 180001022: 75 fc jne 0x180001020
# X64-NEXT: 180001024: c3 retq
## X, a function override after f in its section, moves 7 bytes back.
# X64:      <X>:
# X64-NEXT: 180001025: b8 01 00 00 00 movl $0x1, %eax
## b: boundary padding before its je appears, and the bytes to skip drop the
## padding after it.
# X64:      180001040: 4c 03 35 {{.*}} addq
# X64:      18000105c: 85 c9 testl %ecx, %ecx
# X64-NEXT: 18000105e: 66 90 nop
# X64-NEXT: 180001060: 74 06 je 0x180001068
# X64:      180001068: c3 retq
## k keeps its size and its bytes, its jne over its jmp: the distance from .Lk
## to .Lt would not fit its byte once its padding grew.
# X64:      <k>:
# X64-NEXT: 2e 2e 2e 2e 2e 2e 2e 4c 03 35 {{.*}} addq %cs:
# X64-NEXT: 74 7d je
# X64-NEXT: 75 02 jne
# X64-NEXT: eb 79 jmp

## The spans measure f; an addend 16 bytes into f names its je; the alias.
# X64-DATA:      Contents of section .rdata:
# X64-DATA-NEXT: 180002000 24000000 24
# X64-DATA:      Contents of section .data:
# X64-DATA-NEXT: 180003000 00000000 00000000 20100080 01000000
# X64-DATA-NEXT: 180003010 09100080 01000000

## The Fixup stream records f's movabs where it went, and k's jumps as
## assembled.
# FIXUP:      0001 | {{.*}} | 00001014 | 00001020
# FIXUP:      8022 | 4000  | 0000108F | 0000110D
# FIXUP-NEXT: 8022 | 4000  | 00001091 | 00001094
# FIXUP-NEXT: 8022 | 4000  | 00001093 | 0000110D

# X64-TABLES:      Type: DIR64
# X64-TABLES-NEXT: Address: 0x1014
## pad (listed) and mid (exported) are one address, held once in a guard
## table: with flags in the DLL, without in the executable.
# X64-TABLES:      GuardCFFunctionCount: 1
# X64-TABLES:      GuardFidTable [
# X64-TABLES-NEXT:   0x180001020{{$}}
# X64-TABLES-NEXT: ]
# X64-SET:         GuardCFFunctionCount: 1
# X64-SET:         GuardFidTable [
# X64-SET-NEXT:      0x140001020
# X64-SET-NEXT:    ]
# X64-TABLES:      OriginalRVA: 0x1025

## The adrp and movz go and the padding grows to two words before .La, which
## the cbz still reaches and the span measures.
# A64:      180001000:   ldr x9, 0x180003000
# A64-NEXT: 180001004:   add x9, x9, x28
# A64-NEXT: 180001008:   cbz x0, 0x180001020
# A64-NEXT: 18000100c:   nop
# A64-NEXT: 180001010:   nop
# A64-NEXT: 180001014:   nop
# A64-NEXT: 180001018:   nop
# A64-NEXT: 18000101c:   nop
# A64-NEXT: 180001020:   bl 0x180001000
# A64-NEXT: 180001024:   ret
# A64-DATA: 180002000 20000000

#--- x64.s
  .section .text$a,"xr",discard,f
  .globl f
  .p2align 4
f:
.Lf:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %r14
  addq place(%rip), %r14
  testl %ecx, %ecx
  je .Lpad
  leaq .Lloop(%rip), %rax
  movabsq $.Lloop, %rdx
  .globl pad
  .def pad; .scl 2; .type 32; .endef
pad:
.Lpad:
  .p2align 4
  .globl mid
  .def mid; .scl 2; .type 32; .endef
mid:
.Lloop:
  decl %ecx
  jne .Lloop
.Lend:
  retq
  .globl X
X:
  movl $1, %eax
  retq
  .weak alias
  .set alias, mid

  .section .text$b,"xr",discard,b
  .globl b
  .p2align 5
b:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %r14
  addq place(%rip), %r14
  .fill 21, 1, 0x90
  testl %ecx, %ecx
  je .Lb
  .fill 6, 1, 0x90
.Lb:
  .p2align 4, , 3
  retq

  .section .text$k,"xr",discard,k
  .globl k
  .p2align 4
k:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %r14
  addq place(%rip), %r14
  je .Lt
.Lk:
  jne .Lk1
  jmp .Lt
.Lk1:
  .fill 104, 1, 0x90
  .p2align 4
  .fill 13, 1, 0x90
.Lt:
  retq

  .data
place:
  .quad 0
  .quad alias
  .quad f + 16
  .section .rdata,"dr"
  .long .Lend - .Lf
  .uleb128 .Lend - .Lf
  .byte .Lt - .Lk
  .section .gfids$y,"dr"
  .symidx pad
  .globl @feat.00
@feat.00 = 0x800
  .include "function-override-x.s"
  .rva f, X

#--- a64.s
  .section .text$a,"xr",discard,f
  .globl f
f:
.Lf:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x9, cell
  ldr x9, [x9, :lo12:cell]
  movz x10, #1
  madd x9, x28, x10, x9
  cbz x0, .La
  nop
  nop
  nop
  .p2align 4
.La:
  bl f
  ret
  .data
cell:
  .quad 0
  .section .rdata,"dr"
  .long .La - .Lf

#--- tls.s
  .section .tls$,"dw"
  .long 0
