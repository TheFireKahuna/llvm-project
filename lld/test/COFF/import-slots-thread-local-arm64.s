# REQUIRES: aarch64
## Under -import-slots, an executable's first TLS callback copies each in-place
## import slot in its thread-local template into the main thread's block, which
## the loader copied from the template before it bound the executable's
## imports: only while PEB->CrossProcessFlags says the process is initializing,
## and only a word that still holds its lookup entry.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console tl.obj lib.lib \
# RUN:   -out:tl.exe
# RUN: llvm-readobj --coff-tls-directory tl.exe > tl.txt
# RUN: llvm-objdump -d --print-imm-hex tl.exe >> tl.txt
# RUN: FileCheck --input-file=tl.txt %s

## The slot is at offset 8 in the template, after another thread-local datum;
## the template holds its lookup entry, 0x2060.
# CHECK:      StartAddressOfRawData: 0x[[#%x,START:]]
# CHECK:      AddressOfIndex: 0x[[#%x,INDEX:]]
# CHECK:      ldr x9, [x18, #0x60]
# CHECK-NEXT: ldr w9, [x9, #0x50]
# CHECK-NEXT: tbz w9, #0x1, 0x[[#%x,DONE:]]
# CHECK-NEXT: adrp x9, 0x[[#INDEX]]
# CHECK-NEXT: ldr w9, [x9]
# CHECK-NEXT: ldr x11, [x18, #0x58]
# CHECK-NEXT: ldr x11, [x11, x9, lsl #3]
# CHECK-NEXT: mov w12, #0x8
# CHECK-NEXT: movk w12, #0x0, lsl #16
# CHECK-NEXT: ldr x13, [x11, x12]
# CHECK-NEXT: mov x14, #0x2060
# CHECK-NEXT: movk x14, #0x0, lsl #16
# CHECK-NEXT: movk x14, #0x0, lsl #32
# CHECK-NEXT: movk x14, #0x0, lsl #48
# CHECK-NEXT: cmp x13, x14
# CHECK-NEXT: b.ne 0x[[#DONE]]
# CHECK-NEXT: adrp x10, 0x[[#START]]
# CHECK-NEXT: ldr x10, [x10, #0x8]
# CHECK-NEXT: str x10, [x11, x12]
# CHECK-NEXT: [[#DONE]]: {{.*}} mov w0, #0x0
# CHECK-NEXT: ret

#--- lib.def
LIBRARY lib.dll
EXPORTS
  func

#--- tl.s
  .text
  .globl main
main:
  ret
  .section .tls,"dw"
  .globl _tls_start
_tls_start:
  .quad 0
  .section .tls$,"dw"
  .p2align 3
  .quad func
  .section .tls$ZZZ,"dw"
  .globl _tls_end
_tls_end:
  .quad 0
  .data
  .globl _tls_index
_tls_index:
  .long 0
  .section .rdata,"dr"
  .globl _tls_used
  .p2align 3
_tls_used:
  .quad _tls_start
  .quad _tls_end
  .quad _tls_index
  .quad 0
  .long 0
  .long 0
  .section .CRT$XLA,"dr"
  .globl __xl_a
__xl_a:
  .quad 0
  .section .CRT$XLZ,"dr"
  .globl __xl_z
__xl_z:
  .quad 0
