# REQUIRES: aarch64
## On AArch64, as on x86-64, a DLL's own references to a vague entity it
## keeps preemptible read a cell, which the fill of its overridable-default
## cells writes from the word the executable's table names for the entity's
## key, and which the search of the loaded modules writes from the table of a
## module loaded with the process, or that delay-loads it, once the process
## has initialized, pinning that module and itself.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib -out:a.dll
# RUN: od -A x -t x4 -j 0x30 -N 8 a.dll > a.txt
# RUN: llvm-readobj --coff-imports --unwind a.dll >> a.txt
# RUN: llvm-objdump -d a.dll >> a.txt
# RUN: FileCheck --input-file=a.txt %s

# CHECK:      000030 6c8e9cf5 {{[0-9a-f]+}}

## The search saves the registers that hold its state, and its one epilogue
## restores them.
# CHECK:        0xd20a ; str x27, [sp, #80]
# CHECK-NEXT:   0xc988 ; stp x25, x26, [sp, #64]
# CHECK-NEXT:   0xc906 ; stp x23, x24, [sp, #48]
# CHECK-NEXT:   0xc884 ; stp x21, x22, [sp, #32]
# CHECK-NEXT:   0xc802 ; stp x19, x20, [sp, #16]
# CHECK-NEXT:   0xe1 ; mov fp, sp
# CHECK-NEXT:   0x8d ; stp x29, x30, [sp, #-112]!
# CHECK-NEXT:   0xe4 ; end

# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,NTDLL:]]
# CHECK-NEXT: Symbol: LdrAddRefDll
# CHECK-NEXT: Symbol: NtProtectVirtualMemory

## The reference reads the cell.
# CHECK:      <fa>:
# CHECK-NEXT: adrp x0, 0x[[#%x,CELLPAGE:]]
# CHECK-NEXT: ldr x0, [x0, #0x[[#%x,CELLOFF:]]]

## The fill finds the key among the anchor's entries, loads the word the entry
## names, then the instance from it, and stores it in the cell.
# CHECK:      ldr x17, [x11, #0x8]
# CHECK-NEXT: ldr x17, [x17]
# CHECK-NEXT: adrp x9, 0x[[#CELLPAGE]]
# CHECK-NEXT: add x9, x9, #0x[[#CELLOFF]]
# CHECK-NEXT: str x17, [x9]

## The search: nothing while the process initializes; the modules after the
## executable, each with the table's tag inside its image, probed if loaded
## with the process, if delay-loaded within that scope, or if it delay-loads
## this DLL.
# CHECK:      stp x29, x30, [sp, #-0x70]!
# CHECK:      ldrb w9, [x16, #0x50]
# CHECK-NEXT: tbnz w9, #0x1,
# CHECK:      ldr x26, [x9, #0x18]
# CHECK-NEXT: add x26, x26, #0x10
# CHECK:      ldr x10, [x25, #0x30]
# CHECK:      ldr w11, [x10, #0x30]
# CHECK-NEXT: mov w12, #0x9cf5
# CHECK-NEXT: movk w12, #0x6c8e, lsl #16
# CHECK:      ldr w12, [x25, #0x40]
# CHECK:      ldr w12, [x25, #0x68]
# CHECK-NEXT: tbnz w12, #0x5,
# CHECK:      ldr w12, [x13, #0x10c]
# CHECK-NEXT: cmp w12, #0x3
# CHECK:      ldr x14, [x13, #0xb8]
# CHECK:      ldr x12, [x13, #0x30]
# CHECK-NEXT: cmp x12, x14
## On a hit, LdrAddRefDll(LDR_ADDREF_DLL_PIN, module), then once this DLL.
# CHECK:      ldr x23, [x10, x9]
# CHECK:      mov w0, #0x1
# CHECK-NEXT: mov x1, x10
# CHECK-NEXT: adrp x16, 0x[[#%x,mul(div(NTDLL + 0x180000000, 0x1000), 0x1000)]]
# CHECK-NEXT: ldr x16, [x16, #0x[[#%x,sub(NTDLL, mul(div(NTDLL, 0x1000), 0x1000))]]]
# CHECK-NEXT: blr x16
# CHECK:      mov x1, x27
# CHECK:      str x23, [x27, x9]
# CHECK:      ldp x29, x30, [sp], #0x70
# CHECK-NEXT: ret

#--- a.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl fa
fa:
  adrp x0, __imp__ZN1n1gE
  ldr x0, [x0, :lo12:__imp__ZN1n1gE]
  ret
  .section .drectve,"yni"
  .ascii " -export:fa"

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  NtProtectVirtualMemory
