# REQUIRES: aarch64
## On ARM64 the stub is a branch to its default body, and its fields are
## BRANCH26 fixups: the stub's branch and each direct call or branch to the
## stub. An address load is not patched. A candidate may be a label in the
## stub itself, as the jump through a cell is. A body Windows may select out of
## a branch's range from a site is an error, since Windows does not check.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium fo.s -o fo.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium far.s -o far.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium lc.s -o lc.obj
# RUN: lld-link -machine:arm64 -dll -noentry -export:use fo.obj lc.obj \
# RUN:   -out:fo.dll -map:fo.map
# RUN: cat fo.map > fo.txt
# RUN: llvm-readobj --coff-load-config fo.dll >> fo.txt
# RUN: FileCheck --input-file=fo.txt %s

# CHECK-DAG: {{ }}f.wide [[#%x,WIDE:]] fo.obj
# CHECK-DAG: {{ }}f.base [[#%x,BASE:]] fo.obj
# CHECK-DAG: {{ }}f [[#%x,F:]] fo.obj
# CHECK-DAG: {{ }}use [[#%x,USE:]] fo.obj

# CHECK:      DynamicValueRelocTableSection:
# CHECK:      DynamicRelocations [
# CHECK-NEXT:   Version: 0x1
# CHECK-NEXT:   FunctionOverride [
# CHECK-NEXT:     Entry {
# CHECK-NEXT:       OriginalRVA: 0x[[#%X,BASE-0x180000000]]
# CHECK-NEXT:       RVAs [
# CHECK-NEXT:         RVA: 0x[[#%X,WIDE-0x180000000]]
# CHECK-NEXT:         RVA: 0x[[#%X,F-0x180000000+4]]
# CHECK-NEXT:       ]
# CHECK-NEXT:       Fixups [
# CHECK-NEXT:         Fixup {
# CHECK-NEXT:           Type: ARM64_BRANCH26
# CHECK-NEXT:           RVA: 0x[[#%X,F-0x180000000]]
# CHECK-NEXT:         }
# CHECK-NEXT:         Fixup {
# CHECK-NEXT:           Type: ARM64_BRANCH26
# CHECK-NEXT:           RVA: 0x[[#%X,USE-0x180000000]]
# CHECK-NEXT:         }
# CHECK-NEXT:         Fixup {
# CHECK-NEXT:           Type: ARM64_BRANCH26
# CHECK-NEXT:           RVA: 0x[[#%X,USE-0x180000000+12]]
# CHECK-NEXT:         }
# CHECK-NEXT:       ]
# CHECK-NEXT:       BDD [
# CHECK-NEXT:         Offset: 0x0
# CHECK-NEXT:         Version: 0x1
# CHECK-NEXT:         Node {
# CHECK-NEXT:           Left: 2
# CHECK-NEXT:           Right: 1
# CHECK-NEXT:           Value: 0x10007
# CHECK-NEXT:         }

# RUN: not lld-link -machine:arm64 -dll -noentry -export:use far.obj lc.obj \
# RUN:   -out:far.dll 2>&1 | FileCheck --check-prefix=FAR %s
# FAR: error: the function override of f may select f.far, which is out of range of a branch to it from .text$use at RVA 0x{{[0-9A-F]+}}

#--- fo.s
  .section .text,"xr",one_only,f.wide
  .globl f.wide
f.wide:
  mov w0, #1
  ret

  .section .text,"xr",one_only,f.base
  .globl f.base
f.base:
  mov w0, #0
  ret

  .section .text,"xr",one_only,f
  .globl f
  .p2align 4
f:
  b f.base
  .linkfunctionoverride f, (f.wide, f.cell), (2, 1, 0x10007), (1, 1, 0), (4, 3, 0x10016), (3, 3, 1), (0, 0, 0)
f.cell:
  ret

  .section .text$use,"xr"
  .globl use
use:
  bl f
  adrp x0, f
  add x0, x0, :lo12:f
  b f

#--- far.s
  .section .text,"xr",one_only,f.base
  .globl f.base
f.base:
  ret

  .section .text,"xr",one_only,f
  .globl f
  .p2align 4
f:
  b f.base
  .linkfunctionoverride f, (f.far), (2, 1, 0x10007), (1, 1, 0), (0, 0, 0)

  .section .text$use,"xr"
  .globl use
use:
  b f

  .bss
  .zero 0x8000000

  .section .xfar,"xr"
  .globl f.far
f.far:
  ret

#--- lc.s
  .section .rdata,"dr"
  .globl _load_config_used
  .p2align 3
_load_config_used:
  .long 0x140
  .fill 0x13c, 1, 0
