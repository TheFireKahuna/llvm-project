# REQUIRES: x86
## Under -import-slots, a reference to the record of a thread-local variable
## that the image defines, X$tls, or to the record's import pointer, reaches a
## record that the image writes where no input defines one: the address of the
## image's TLS index, the variable's offset in the image's TLS template, and the
## offset of the byte that guards its initialization, which has the name the
## Itanium ABI gives the variable's guard. A variable with neither a guard nor
## an init function, whose guard is then never tested, names itself. A
## described load of the import pointer reaches the record directly. A record
## is kept only if something live refers to it, and keeps its variable and its
## guard.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tls.s -o tls.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium index.s -o index.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dead.s -o dead.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium noguard.s \
# RUN:   -o noguard.obj

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj tls.obj \
# RUN:   index.obj -out:main.exe
# RUN: llvm-readobj --sections --coff-basereloc main.exe > main.txt
# RUN: llvm-objdump -d main.exe >> main.txt
# RUN: llvm-objdump -s -j .rdata main.exe >> main.txt
# RUN: FileCheck --input-file=main.txt %s

## The records, laid out by their variables' names, each hold an address the
## loader relocates.
# CHECK:      Name: .rdata
# CHECK-NEXT: VirtualSize: 0x20
# CHECK-NEXT: VirtualAddress: 0x[[#%x,RDATA:]]
# CHECK:      Name: .data
# CHECK-NEXT: VirtualSize: 0x4
# CHECK-NEXT: VirtualAddress: 0x3000
# CHECK:      Address: 0x[[#RDATA]]
# CHECK:      Address: 0x[[#RDATA + 0x10]]

## The loads of the records' pointers reach the records; plain's is referred to
## by name too.
# CHECK:      <.text>:
# CHECK-NEXT: leaq {{.*}}(%rip), %rax # 0x[[#%x,0x140000000 + RDATA + 0x10]]
# CHECK-NEXT: leaq {{.*}}(%rip), %rcx # 0x[[#%x,0x140000000 + RDATA]]
# CHECK-NEXT: leaq {{.*}}(%rip), %rdx # 0x[[#%x,0x140000000 + RDATA]]

## Each record holds the address of _tls_index, at 0x140003000. plain, at
## offset 8 of the template, names itself; strong is at 0 and its guard at 4.
# CHECK:      Contents of section .rdata:
# CHECK-NEXT: [[#%x,0x140000000 + RDATA]] 00300040 01000000 08000000 08000000
# CHECK-NEXT: [[#%x,0x140000000 + RDATA + 0x10]] 00300040 01000000 00000000 04000000

## The variable that only a dead function's record names is left out of the
## template, which holds strong, its guard and plain.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj tls.obj \
# RUN:   index.obj dead.obj -out:dead.exe
# RUN: llvm-readobj --sections dead.exe | FileCheck --check-prefix=DEAD %s
# DEAD:      Name: .tls
# DEAD-NEXT: VirtualSize: 0xC

## A record needs the image's TLS index.
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   tls.obj -out:noindex.exe 2>&1 | FileCheck --check-prefix=NOINDEX %s
# NOINDEX: error: the record of thread-local variable strong needs the image's TLS index, _tls_index, which nothing defines

## A variable whose init function exists needs its guard named.
# RUN: not lld-link -import-slots -entry:dyn -subsystem:console noguard.obj \
# RUN:   index.obj -out:noguard.exe 2>&1 | FileCheck --check-prefix=NOGUARD %s
# NOGUARD: error: noguard.obj: the record of thread-local variable dyn needs the byte that guards its initialization, which nothing names _ZGV3dyn

#--- main.s
  .text
  .globl main
main:
  movq "__imp_strong$tls"(%rip), %rax
  movq "__imp_plain$tls"(%rip), %rcx
  leaq "plain$tls"(%rip), %rdx
  retq

#--- tls.s
  .section .tls$,"dw"
  .p2align 2
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

  .section .tls$,"dw",discard,unused
  .globl unused
unused:
  .long 3

  .text
  .globl _ZTH6strong
_ZTH6strong:
  retq

#--- dead.s
  .section .text,"xr",one_only,dead
  .globl dead
dead:
  movq "__imp_unused$tls"(%rip), %rax
  retq

#--- index.s
  .data
  .globl _tls_index
_tls_index:
  .long 0

#--- noguard.s
  .section .tls$,"dw"
  .globl dyn
dyn:
  .long 0

  .text
  .globl _ZTH3dyn
_ZTH3dyn:
  retq
  .globl entry
entry:
  movq "__imp_dyn$tls"(%rip), %rax
  retq
