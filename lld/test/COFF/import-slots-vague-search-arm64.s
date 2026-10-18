# REQUIRES: aarch64
## On AArch64, as on x86-64, a DLL's own references to a vague entity it
## keeps preemptible read a cell, which the fill of its overridable-default
## cells writes from the word the executable's table names for the entity's
## key, and which the search of the loaded modules writes from the table of a
## module loaded with the process, or that delay-loads it, once the process
## has initialized, pinning that module but not itself.

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
# CHECK-NEXT:   0x91 ; stp x29, x30, [sp, #-144]!
# CHECK-NEXT:   0xe4 ; end

# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,NTDLL:]]
# CHECK-NEXT: Symbol: LdrAddRefDll
# CHECK-NEXT: Symbol: NtProtectVirtualMemory

## The reference reads the cell.
# CHECK:      <fa>:
# CHECK-NEXT: adrp x0
# CHECK-NEXT: ldr x0, [x0

## Bit 2 of w24 records a DLL loaded after the process initialized. For an
## entity the executable's table does not name, such a DLL searches the images
## of its scope, the executable first through the table it names at 0x38, then
## each module with a table's tag inside its image, probed if loaded with the
## process, if delay-loaded within that scope, or if it delay-loads this DLL.
# CHECK:      stp x29, x30, [sp, #-0x90]!
# CHECK:      mov w24, #0x0
# CHECK-NEXT: adrp x27, 0x180000000
# CHECK-NEXT: add x27, x27, #0x0
# CHECK-NEXT: ldr x16, [x18, #0x60]
# CHECK-NEXT: ldrb w9, [x16, #0x50]
# CHECK-NEXT: tbnz w9, #0x1,
# CHECK-NEXT: orr w24, w24, #0x4
# CHECK:      ldr x22, [x19]
# CHECK-NEXT: and x22, x22, #0xfffffffffffffffe
# CHECK:      ldr w9, [x19, #0xc]
# CHECK-NEXT: cmp w9, #0x1, lsl #12
# CHECK-NEXT: b.lo
# CHECK-NEXT: tbz w24, #0x2,
# CHECK:      ldr x26, [x9, #0x18]
# CHECK-NEXT: add x26, x26, #0x10
# CHECK-NEXT: ldr x25, [x26]
# CHECK:      ldr x10, [x25, #0x30]
# CHECK:      ldr w11, [x10, #0x38]
# CHECK:      ldr w11, [x10, #0x30]
# CHECK-NEXT: mov w12, #0x9cf5
# CHECK-NEXT: movk w12, #0x6c8e, lsl #16
# CHECK:      ldr w12, [x25, #0x40]
# CHECK:      mov w13, #0x1d2b
# CHECK-NEXT: movk w13, #0x4f7a, lsl #16
# CHECK:      ldr w12, [x25, #0x68]
# CHECK-NEXT: tbnz w12, #0x5,
# CHECK:      ldr w12, [x13, #0x10c]
# CHECK-NEXT: cmp w12, #0x3
# CHECK:      ldr x14, [x13, #0xb8]
# CHECK:      ldr x12, [x13, #0x30]
# CHECK-NEXT: cmp x12, x14
## Two of the key's bits in the filter's word.
# CHECK:      lsr x12, x22, #13
# CHECK:      ldr x12, [x12, #0x20]
# CHECK:      lsr x14, x12, x13
# CHECK-NEXT: tbz w14, #0x0,
## A bucket's key counts only if its name is the entity's, up to its
## terminator.
# CHECK:      ldrb w16, [x12], #0x1
# CHECK-NEXT: ldrb w17, [x14], #0x1
# CHECK-NEXT: cmp w16, w17
## The executable's entry names the instance itself, a DLL's a word holding
## it. On a hit in a DLL, LdrAddRefDll(LDR_ADDREF_DLL_PIN, module), whose
## failure ends the search with its status.
# CHECK:      add x23, x10, x9
# CHECK-NEXT: ldr w12, [x11, #0x4]
# CHECK-NEXT: tbnz w12, #0x0,
# CHECK-NEXT: ldr x23, [x23]
# CHECK:      mov w0, #0x1
# CHECK-NEXT: mov x1, x10
# CHECK-NEXT: adrp x16, 0x[[#%x,mul(div(NTDLL + 0x180000000, 0x1000), 0x1000)]]
# CHECK-NEXT: ldr x16, [x16, #0x[[#%x,sub(NTDLL, mul(div(NTDLL, 0x1000), 0x1000))]]]
# CHECK-NEXT: blr x16
# CHECK-NEXT: tbnz w0, #0x1f,
# CHECK:      ldr w9, [x19]
# CHECK-NEXT: tbz w9, #0x0,
# CHECK:      str x23, [x27, x9]
# CHECK-NEXT: add x19, x19, #0x10
## An entity the table names takes the word its entry names, then the
## instance from it.
# CHECK:      ldr x23, [x11, #0x8]
# CHECK-NEXT: ldr w9, [x19, #0xc]
# CHECK-NEXT: cmp w9, #0x1
# CHECK-NEXT: b.eq
# CHECK-NEXT: cmp w9, #0x1, lsl #12
# CHECK-NEXT: b.lo
# CHECK-NEXT: ldr x23, [x23]
# CHECK-NEXT: b
## Nothing pins the DLL itself.
# RUN: FileCheck --check-prefix=NOSELF --input-file=a.txt %s
# NOSELF-NOT: mov x1, x27
# CHECK:      ldp x29, x30, [sp], #0x90
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
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
