# REQUIRES: aarch64
## Under -import-slots, an executable's C initializer table runs a copy of each
## in-place import slot in its thread-local template into the main thread's
## block, which the loader copied from the template before it bound the
## executable's imports.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console tl.obj lib.lib \
# RUN:   -out:tl.exe
# RUN: llvm-readobj --coff-tls-directory tl.exe > tl.txt
# RUN: llvm-objdump -d --print-imm-hex tl.exe >> tl.txt
# RUN: FileCheck --input-file=tl.txt %s

## The slot is at offset 8 in the template, after another thread-local datum.
# CHECK:      StartAddressOfRawData: 0x[[#%x,START:]]
# CHECK:      AddressOfIndex: 0x[[#%x,INDEX:]]
# CHECK:      adrp x9, 0x[[#INDEX]]
# CHECK-NEXT: ldr w9, [x9]
# CHECK-NEXT: ldr x11, [x18, #0x58]
# CHECK-NEXT: ldr x11, [x11, x9, lsl #3]
# CHECK-NEXT: adrp x10, 0x[[#START]]
# CHECK-NEXT: ldr x10, [x10, #0x8]
# CHECK-NEXT: mov w12, #0x8
# CHECK-NEXT: movk w12, #0x0, lsl #16
# CHECK-NEXT: str x10, [x11, x12]
# CHECK-NEXT: mov w0, #0x0
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
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0
