# REQUIRES: x86
## Where the record of a thread-local variable is in the image, the linker
## knows its words, and code whose object describes the loads from it reaches
## the variable as code of the image reaches its own: the record's load goes,
## the load of the address of the TLS index goes and the index is read
## RIP-relative, and the load of the variable's or its guard's offset goes,
## each memory operand that added it adding it to its displacement instead. A
## load whose readers are not all described stays, the address becoming an lea
## of the index and the offset a move of it. A record the image writes that
## nothing reads any longer is left out; one an object defines stays. Code
## where a value would no longer fit keeps its size and reads the record.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tls.s -o tls.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj tls.obj \
# RUN:   -out:main.exe
# RUN: llvm-readobj --sections main.exe > main.txt
# RUN: llvm-objdump -d main.exe >> main.txt
# RUN: llvm-objdump -s -j .rdata main.exe >> main.txt
# RUN: FileCheck --input-file=main.txt %s

## The template holds 8 bytes of padding, strong at 8 and its guard at 12,
## plain at 16, own at 20 and late at 24. _tls_index is at 0x140003000.

## Of the records, .rdata keeps own's, after a byte of data, and late's.
# CHECK:      Name: .rdata
# CHECK-NEXT: VirtualSize: 0x28

## The memory operands that added strong's offset, or its guard's, add it to
## their displacements, keeping the other register as their base: with a SIB
## byte where that is R12 or RSP, R12's high bit moving to VEX's inverted B.
# CHECK:      <.text>:
# CHECK:      8b 0d {{.*}} movl {{.*}}(%rip), %ecx # 0x140003000
# CHECK-NEXT: 65 48 8b 14 25 58 00 00 00 movq %gs:0x58, %rdx
# CHECK-NEXT: 48 8b 0c ca movq (%rdx,%rcx,8), %rcx
# CHECK-NEXT: 40 8b 81 0c 00 00 00 movl 0xc(%rcx), %eax
# CHECK-NEXT: c4 c1 7a 10 84 24 08 00 00 00 vmovss 0x8(%r12), %xmm0
# CHECK-NEXT: 48 8d 94 24 08 00 00 00 leaq 0x8(%rsp), %rdx
# CHECK-NEXT: 40 80 b9 0c 00 00 00 00 cmpb $0x0, 0xc(%rcx)
# CHECK-NEXT: c3 retq

## A value that the longer load of the index would take past 255 keeps
## fallback's size: the record's load computes its address, and late's record
## stays.
# CHECK-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x140002018
# CHECK-NEXT: 48 8b 08 movq (%rax), %rcx
# CHECK-NEXT: 8b 09 movl (%rcx), %ecx

## Code reaches own directly too, though its record, which an object defines,
## stays.
# CHECK:      c3 retq
# CHECK-NEXT: 8b 0d {{.*}} movl {{.*}}(%rip), %ecx # 0x140003000
# CHECK-NEXT: 65 48 8b 14 25 58 00 00 00 movq %gs:0x58, %rdx
# CHECK-NEXT: 48 8b 0c ca movq (%rdx,%rcx,8), %rcx
# CHECK-NEXT: 8b 81 14 00 00 00 movl 0x14(%rcx), %eax
# CHECK-NEXT: c3 retq

## In partial, an instruction not described reads each load from plain's
## record: the first computes the address of the TLS index and the second moves
## plain's offset, so the record is left out too.
# CHECK-NEXT: 4c 8d 1d {{.*}} leaq {{.*}}(%rip), %r11 # 0x140003000
# CHECK-NEXT: 41 8b 0b movl (%r11), %ecx
# CHECK-NEXT: 65 48 8b 14 25 58 00 00 00 movq %gs:0x58, %rdx
# CHECK-NEXT: 41 b8 10 00 00 00 movl $0x10, %r8d
# CHECK-NEXT: 4c 03 04 ca addq (%rdx,%rcx,8), %r8

# CHECK:      Contents of section .rdata:
# CHECK-NEXT: 140002000 ff003000 40010000 00140000 00140000
# CHECK-NEXT: 140002010 00000000 00000000 00300040 01000000
# CHECK-NEXT: 140002020 18000000 18000000

#--- main.s
  .text
  .globl main
main:
  retq

  .section .text$direct,"xr"
  .globl direct
direct:
.La:
  .linkfusedread
  movq "__imp_strong$tls"(%rip), %rax
  .linkfusedaccess .La, %rax
.Lb:
  .linkfusedread
  movq (%rax), %rcx
  .linkfusedaccess .Lb, %rcx
  movl (%rcx), %ecx
  movq %gs:88, %rdx
  movq (%rdx,%rcx,8), %rcx
  .linkfusedaccess .La, %rax
.Lc:
  .linkfusedread
  movl 8(%rax), %r9d
  .linkfusedindex .Lc, %r9
  movl 4(%rcx,%r9), %eax
  .linkfusedindex .Lc, %r9
  vmovss (%r9,%r12), %xmm0
  .linkfusedindex .Lc, %r9
  leaq (%rsp,%r9), %rdx
  .linkfusedaccess .La, %rax
.Ld:
  .linkfusedread
  movl 12(%rax), %r10d
  .linkfusedindex .Ld, %r10
  cmpb $0, (%rcx,%r10)
  retq

  .section .text$partial,"xr"
  .globl partial
partial:
.Le:
  .linkfusedread
  movq "__imp_plain$tls"(%rip), %rax
  .linkfusedaccess .Le, %rax
  movq (%rax), %r11
  movl (%r11), %ecx
  movq %gs:88, %rdx
  .linkfusedaccess .Le, %rax
  movl 8(%rax), %r8d
  addq (%rdx,%rcx,8), %r8
  movl (%r8), %eax
  retq

  .section .text$own,"xr"
  .globl ownaccess
ownaccess:
.Lf:
  .linkfusedread
  movq "__imp_own$tls"(%rip), %rax
  .linkfusedaccess .Lf, %rax
.Lg:
  .linkfusedread
  movq (%rax), %rcx
  .linkfusedaccess .Lg, %rcx
  movl (%rcx), %ecx
  movq %gs:88, %rdx
  movq (%rdx,%rcx,8), %rcx
  .linkfusedaccess .Lf, %rax
.Lh:
  .linkfusedread
  movl 8(%rax), %eax
  .linkfusedindex .Lh, %rax
  movl (%rcx,%rax), %eax
  retq

  .section .text$fb,"xr"
  .globl fallback
fallback:
.Li:
  .linkfusedread
  movq "__imp_late$tls"(%rip), %rax
.Lfbstart:
  .linkfusedaccess .Li, %rax
.Lj:
  .linkfusedread
  movq (%rax), %rcx
  .linkfusedaccess .Lj, %rcx
  movl (%rcx), %ecx
  .fill 250, 1, 0x90
.Lfbend:
  retq

  .section .rdata,"dr"
  .byte .Lfbend - .Lfbstart

#--- tls.s
  .section .tls$,"dw"
  .p2align 2
  .long 0, 0
  .globl strong
strong:
  .long 1
  .globl _ZGV6strong
_ZGV6strong:
  .byte 0
  .p2align 2
  .globl plain
plain:
  .long 2
  .globl own
own:
  .long 3
  .globl late
late:
  .long 4

  .text
  .globl _ZTH6strong
_ZTH6strong:
  retq

  .section .rdata,"dr"
  .globl "own$tls"
"own$tls":
  .quad _tls_index
  .secrel32 own
  .secrel32 own

  .data
  .globl _tls_index
_tls_index:
  .long 0
