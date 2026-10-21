# REQUIRES: x86
## A function-override link record makes an entry of the dynamic value
## relocation table's function-override record: the body the stub jumps to,
## the bodies Windows may select instead, the stub's jump and each direct call
## or jump to the stub as the fields it writes, and the diagram. The record
## keeps the bodies, and two stubs stay two sites, however alike.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium fo.s -o fo.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lc.s -o lc.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium bad.s -o bad.obj
# RUN: lld-link -dll -noentry -opt:ref,icf -export:use fo.obj lc.obj \
# RUN:   -out:fo.dll -map:fo.map
# RUN: cat fo.map > fo.txt
# RUN: llvm-readobj --coff-load-config fo.dll >> fo.txt
# RUN: FileCheck --input-file=fo.txt %s

# CHECK-DAG: {{ }}f.wide [[#%x,WIDE:]] fo.obj
# CHECK-DAG: {{ }}f.narrow [[#%x,NARROW:]] fo.obj
# CHECK-DAG: {{ }}f.base [[#%x,BASE:]] fo.obj
# CHECK-DAG: {{ }}f [[#%x,F:]] fo.obj
# CHECK-DAG: {{ }}g [[#%x,G:]] fo.obj
# CHECK-DAG: {{ }}use [[#%x,USE:]] fo.obj

# CHECK:      DynamicValueRelocTableSection:
# CHECK:      DynamicRelocations [
# CHECK-NEXT:   Version: 0x1
# CHECK-NEXT:   FunctionOverride [
# CHECK-NEXT:     Entry {
# CHECK-NEXT:       OriginalRVA: 0x[[#%X,BASE-0x180000000]]
# CHECK-NEXT:       RVAs [
# CHECK-NEXT:         RVA: 0x[[#%X,WIDE-0x180000000]]
# CHECK-NEXT:         RVA: 0x[[#%X,NARROW-0x180000000]]
# CHECK-NEXT:       ]
# CHECK-NEXT:       Fixups [
# CHECK-NEXT:         Fixup {
# CHECK-NEXT:           Type: X64_REL32
# CHECK-NEXT:           RVA: 0x[[#%X,F-0x180000000+1]]
# CHECK-NEXT:         }
# CHECK-NEXT:         Fixup {
# CHECK-NEXT:           Type: X64_REL32
# CHECK-NEXT:           RVA: 0x[[#%X,USE-0x180000000+1]]
# CHECK-NEXT:         }
# CHECK-NEXT:       ]
# CHECK-NEXT:       BDD [
# CHECK-NEXT:         Version: 0x1
# CHECK-NEXT:         Node {
# CHECK-NEXT:           Left: 2
# CHECK-NEXT:           Right: 1
# CHECK-NEXT:           Value: 0x13E
# CHECK-NEXT:         }
# CHECK:            ]
# CHECK-NEXT:     }
# CHECK-NEXT:     Entry {
# CHECK-NEXT:       OriginalRVA: 0x[[#%X,BASE-0x180000000]]
# CHECK-NEXT:       RVAs [
# CHECK-NEXT:         RVA: 0x[[#%X,WIDE-0x180000000]]
# CHECK-NEXT:         RVA: 0x[[#%X,NARROW-0x180000000]]
# CHECK-NEXT:       ]
# CHECK-NEXT:       Fixups [
# CHECK-NEXT:         Fixup {
# CHECK-NEXT:           Type: X64_REL32
# CHECK-NEXT:           RVA: 0x[[#%X,G-0x180000000+1]]
# CHECK-NEXT:         }
# CHECK-NEXT:         Fixup {
# CHECK-NEXT:           Type: X64_REL32
# CHECK-NEXT:           RVA: 0x[[#%X,USE-0x180000000+13]]
# CHECK-NEXT:         }
# CHECK-NEXT:       ]

## A record whose diagram has an edge leading back, or whose stub does not
## jump to a body, is an error.
# RUN: not lld-link -dll -noentry -export:use bad.obj lc.obj -out:bad.dll 2>&1 \
# RUN:   | FileCheck --check-prefix=BAD %s
# BAD-DAG: error: bad.obj: the function override of loops has a malformed diagram
# BAD-DAG: error: bad.obj: the function override of returns names a stub that does not jump to a body

#--- fo.s
  .section .text,"xr",one_only,f.wide
  .globl f.wide
f.wide:
  movl $2, %eax
  retq

  .section .text,"xr",one_only,f.narrow
  .globl f.narrow
f.narrow:
  movl $1, %eax
  retq

  .section .text,"xr",one_only,f.base
  .globl f.base
f.base:
  xorl %eax, %eax
  retq

## f and g are alike in every byte and relocation.
  .section .text,"xr",one_only,f
  .globl f
  .p2align 4
f:
  jmp f.base
  .linkfunctionoverride f, (f.wide, f.narrow), (2, 1, 0x13e), (1, 1, 0), (4, 3, 1), (3, 3, 1), (0, 0, 0)

  .section .text,"xr",one_only,g
  .globl g
  .p2align 4
g:
  jmp f.base
  .linkfunctionoverride g, (f.wide, f.narrow), (2, 1, 0x13e), (1, 1, 0), (4, 3, 1), (3, 3, 1), (0, 0, 0)

## A direct call is patched too; an address load is not. A call through the
## stub's import pointer is patched once the linker makes it direct.
  .section .text$use,"xr"
  .globl use
use:
  callq f
  leaq g(%rip), %rax
  callq *__imp_g(%rip)
  retq

#--- bad.s
  .section .text,"xr",one_only,body
  .globl body
body:
  retq

  .section .text,"xr",one_only,loops
  .globl loops
loops:
  jmp body
  .linkfunctionoverride loops, (body), (0, 1, 0x13e), (1, 1, 0)

  .section .text,"xr",one_only,returns
  .globl returns
returns:
  retq
  .linkfunctionoverride returns, (body), (1, 1, 0)

  .section .text$use,"xr"
  .globl use
use:
  callq loops
  callq returns
  retq

#--- lc.s
  .section .rdata,"dr"
  .globl _load_config_used
  .p2align 3
_load_config_used:
  .long 0x140
  .fill 0x13c, 1, 0
