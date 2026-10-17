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

## A DLL that imports from an executable runs the copy the executable names
## from its C initializer table, after the guard's check, in a frame its unwind
## information describes.
# RUN: lld-link -def:host.def -out:host.lib -machine:arm64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium plug.s -o plug.obj
# RUN: lld-link -import-slots -dll -noentry plug.obj host.lib ntdll.lib \
# RUN:   -out:plug.dll
# RUN: llvm-objdump -d --print-imm-hex plug.dll > plug.txt
# RUN: llvm-readobj --unwind plug.dll >> plug.txt
# RUN: FileCheck --check-prefix=CALL --input-file=plug.txt %s
# CALL:      180001010: {{.*}} stp x29, x30, [sp, #-0x30]!
# CALL-NEXT: mov x29, sp
# CALL-NEXT: ldr x9, [x18, #0x60]
# CALL-NEXT: ldr w10, [x9, #0x50]
# CALL-NEXT: tbz w10, #0x1, 0x[[#%x,ZERO:]]
# CALL-NEXT: ldr x9, [x9, #0x10]
# CALL-NEXT: ldr w10, [x9, #0x1c]
# CALL-NEXT: mov w11, #0x79b9
# CALL-NEXT: movk w11, #0x9e37, lsl #16
# CALL-NEXT: cmp w10, w11
# CALL-NEXT: b.ne 0x[[#ZERO]]
# CALL-NEXT: ldr w10, [x9, #0x20]
# CALL-NEXT: add x15, x9, x10
# CALL-NEXT: adrp x16, 0x180002000
# CALL-NEXT: ldr x16, [x16]
# CALL-NEXT: blr x16
# CALL-NEXT: blr x15
# CALL-NEXT: [[#ZERO]]: {{.*}} mov w0, #0x0
# CALL-NEXT: mov sp, x29
# CALL-NEXT: ldp x29, x30, [sp], #0x30
# CALL-NEXT: ret
# CALL:      Function: 0x180001010
# CALL:      Prologue [
# CALL-NEXT:   0xe1 ; mov fp, sp
# CALL-NEXT:   0x85 ; stp x29, x30, [sp, #-48]!
# CALL-NEXT:   0xe4 ; end

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory

#--- host.def
LIBRARY host.exe
EXPORTS
  hostfn

#--- plug.s
  .text
  .globl dllmain
dllmain:
  b hostfn
## The guard's check pointer, as the load configuration provides it, at the
## start of .rdata.
  .section .rdata,"dr"
  .globl __guard_check_icall_fptr
  .p2align 3
__guard_check_icall_fptr:
  .quad 0
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

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
