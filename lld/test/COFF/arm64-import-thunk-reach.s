# REQUIRES: aarch64

## In an image with a fork object, an ARM64 import thunk reads its pointer
## with one ldr (literal), and a delay-load thunk takes the address of its
## pointer with one adr, where the pointer is within 1 MB; each thunk whose
## pointer the layout puts further takes an adrp and the ldr or add.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: lld-link -machine:arm64 -def:imp.def -out:imp.lib
# RUN: lld-link -machine:arm64 -def:dly.def -out:dly.lib
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium far.s -o far.obj
# DEFINE: %{link} = lld-link -dll -noentry -include:main imp.lib dly.lib \
# DEFINE:   main.obj -delayload:dly.dll
# RUN: %{link} -out:near.dll
# RUN: llvm-objdump -d --no-show-raw-insn near.dll | FileCheck --check-prefix=NEAR %s
# RUN: %{link} far.obj -out:far.dll
# RUN: llvm-objdump -d --no-show-raw-insn far.dll | FileCheck --check-prefix=FAR %s

# NEAR:      180001010: ldr x16, 0x180002000
# NEAR-NEXT:            br x16
# NEAR-NEXT:            ldr x16, 0x180003008
# NEAR-NEXT:            br x16
# NEAR-NEXT:            adr x17, 0x180003008
# NEAR-NEXT:            b 0x180001028

## f's entry stays near, before the 1 MB that pushes .data away.
# FAR:      180001010: ldr x16, 0x180002000
# FAR-NEXT:            br x16
# FAR-NEXT:            adrp x16, 0x180103000
# FAR-NEXT:            ldr x16, [x16, #0x8]
# FAR-NEXT:            br x16
# FAR-NEXT:            adrp x17, 0x180103000
# FAR-NEXT:            add x17, x17, #0x8
# FAR-NEXT:            b 0x180001030

#--- imp.def
LIBRARY imp.dll
EXPORTS
  f

#--- dly.def
LIBRARY dly.dll
EXPORTS
  g

#--- main.s
  .globl main
main:
  bl f
  bl g
  ret

  .globl __delayLoadHelper2
__delayLoadHelper2:
  ret

#--- far.s
  .section .rdata,"dr"
  .fill 0x100000, 1, 0
