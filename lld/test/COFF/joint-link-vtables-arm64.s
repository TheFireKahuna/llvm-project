# REQUIRES: aarch64
## On ARM64, a member that gives up a vtable whose exact casts compare vtable
## pointers for another member's turns the adrp and add of its address point
## into an adrp and ldr of the import of the name the owner exports for that
## address point; a copy of any other vtable stays.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium b.s -o b.obj
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: llvm-readobj --coff-imports b.dll > b.txt
# RUN: llvm-objdump -d --no-show-raw-insn b.dll >> b.txt
# RUN: FileCheck %s < b.txt

# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,IAT:]]
# CHECK-NEXT: Symbol: _ZTV1D$ap16 (0)
# CHECK-NEXT: }
# CHECK:      <bf>:
# CHECK-NEXT: adrp x0, 0x[[#%x,mul(div(IAT+6442450944,4096),4096)]]
# CHECK-NEXT: ldr x0, [x0, #0x[[#%x,IAT-mul(div(IAT,4096),4096)]]]
# CHECK-NEXT: adrp x1,
# CHECK-NEXT: add x1, x1,
# CHECK-NEXT: ret

#--- a.s
  .section .rdata,"dr",discard,_ZTV1D
  .globl _ZTV1D
  .p2align 6
_ZTV1D:
  .xword 0
  .xword 0
  .xword af
  .globl _ZTV1D$ap16
_ZTV1D$ap16 = _ZTV1D+16

  .text
  .globl af
af:
  adrp x0, _ZTV1D+16
  add x0, x0, :lo12:_ZTV1D+16
  ret

  .section .drectve,"yni"
  .ascii " -export:af /exactcast:_ZTV1D"

#--- b.s
  .section .rdata,"dr",discard,_ZTV1D
  .globl _ZTV1D
  .p2align 6
_ZTV1D:
  .xword 0
  .xword 0
  .xword bf
  .globl _ZTV1D$ap16
_ZTV1D$ap16 = _ZTV1D+16

  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 6
_ZTV1C:
  .xword 0
  .xword 0
  .xword bf
  .globl _ZTV1C$ap16
_ZTV1C$ap16 = _ZTV1C+16

  .text
  .globl bf
bf:
  adrp x0, _ZTV1D+16
  add x0, x0, :lo12:_ZTV1D+16
  adrp x1, _ZTV1C+16
  add x1, x1, :lo12:_ZTV1C+16
  ret

  .section .drectve,"yni"
  .ascii " -export:bf /exactcast:_ZTV1D"
