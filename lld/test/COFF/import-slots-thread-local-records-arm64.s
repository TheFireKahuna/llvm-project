# REQUIRES: aarch64
## On AArch64, as on x86-64, the import pointer of the record of a
## thread-local variable that the image defines is a record the image writes:
## the address of the image's TLS index, the variable's offset in the template
## and that of its guard. The adrp and ldr of the pointer become adrp and add
## of the record.

# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium %s -o %t.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console %t.obj -out:%t.exe
# RUN: llvm-objdump -d %t.exe > %t.txt
# RUN: llvm-objdump -s -j .rdata %t.exe >> %t.txt
# RUN: llvm-readobj --coff-basereloc %t.exe >> %t.txt
# RUN: FileCheck --input-file=%t.txt %s

# CHECK:      <.text>:
# CHECK-NEXT: adrp x0, 0x[[#%x,REC:]]
# CHECK-NEXT: add x0, x0, #0x0

## _tls_index is at 0x140003000, strong at offset 0 and its guard at 4.
# CHECK:      Contents of section .rdata:
# CHECK-NEXT: [[#%x,REC]] 00300040 01000000 00000000 04000000

# CHECK:      Type: DIR64
# CHECK-NEXT: Address: 0x[[#%x,REC - 0x140000000]]

  .text
  .globl main
main:
  adrp x0, "__imp_strong$tls"
  ldr x0, [x0, :lo12:"__imp_strong$tls"]
  ret

  .section .tls$,"dw"
  .p2align 2
  .globl strong
strong:
  .long 1
  .globl _ZGV6strong
_ZGV6strong:
  .byte 0

  .text
  .globl _ZTH6strong
_ZTH6strong:
  ret

  .data
  .globl _tls_index
_tls_index:
  .long 0
