# REQUIRES: x86, aarch64

## A BRANCH_TAIL that ends its chunk where its target's chunk follows is
## deleted, padding becoming no-operations; it stays (two bytes on x86-64) with
## an addend, an inner symbol, an override target or a fixed-size section.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{x64} = llvm-mc -filetype=obj -triple=x86_64-windows-itanium
# DEFINE: %{a64} = llvm-mc -filetype=obj -triple=aarch64-windows-itanium
# RUN: %{x64} x64.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc x64.s -o foreign.obj
# RUN: %{a64} a64.s -o a64.obj
# RUN: %{a64} a64-thunk.s -o a64-thunk.obj
# RUN: %{a64} a64-restart.s -o a64-restart.obj
# RUN: %{x64} -I %S/Inputs fo.s -o fo.obj
# RUN: %{x64} fw.s -o fw.obj
# RUN: %{x64} --defsym=TAKEN=1 fw.s -o fw-taken.obj
# RUN: %{x64} --defsym=LISTED=1 fw.s -o fw-listed.obj
# RUN: %{x64} %S/Inputs/loadconfig-cfg-x64.s -o loadconfig.obj
# DEFINE: %{link} = lld-link -dll -noentry -export:f
# DEFINE: %{dis} = llvm-objdump -d --no-show-raw-insn

# RUN: %{link} -debug -debugtype:fixup -pdb:x64.pdb x64.obj -out:x64.dll
# RUN: %{dis} x64.dll | FileCheck --check-prefix=X64 %s
# RUN: llvm-objdump -d x64.dll | FileCheck --check-prefix=GAP %s
# RUN: llvm-readobj --unwind x64.dll | FileCheck --check-prefix=PDATA %s
# RUN: llvm-pdbutil dump -fixups x64.pdb | FileCheck --check-prefix=FIXUP %s
# RUN: %{link} foreign.obj -out:foreign.dll
# RUN: %{dis} foreign.dll | FileCheck --check-prefix=FOREIGN %s
# RUN: %{link} -machine:arm64 a64.obj -out:a64.dll
# RUN: %{dis} a64.dll | FileCheck --check-prefix=A64 %s
# RUN: lld-link -machine:arm64 -entry:main -subsystem:console a64-thunk.obj \
# RUN:   -out:a64-thunk.exe
# RUN: %{dis} a64-thunk.exe | FileCheck --check-prefix=A64-THUNK %s
# RUN: lld-link -machine:arm64 -entry:main -subsystem:console a64-restart.obj \
# RUN:   -out:a64-restart.exe -verbose 2>&1 | FileCheck --check-prefix=RESTART-LOG %s
# RUN: %{dis} a64-restart.exe | FileCheck --check-prefix=RESTART %s
# RUN: lld-link -dll -noentry -export:X -funcoverride -guard:cf fo.obj \
# RUN:   loadconfig.obj -out:fo.dll
# RUN: %{dis} fo.dll | FileCheck --check-prefix=FO %s
# DEFINE: %{fw} = lld-link -dll -noentry -include:f -export:g -guard:cf \
# DEFINE:   loadconfig.obj
# RUN: %{fw} fw.obj -out:fw.dll
# RUN: %{dis} fw.dll | FileCheck --check-prefix=FW %s
# RUN: %{fw} fw-taken.obj -out:fw-taken.dll
# RUN: %{dis} fw-taken.dll | FileCheck --check-prefix=FW-TAKEN %s
# RUN: %{fw} fw-listed.obj -out:fw-listed.dll
# RUN: %{dis} fw-listed.dll | FileCheck --check-prefix=FW %s
# RUN: llvm-readobj --coff-load-config fw-listed.dll | FileCheck --check-prefix=FW-FIDS %s
# RUN: %{fw} -opt:noicf fw.obj -out:fw-noicf.dll
# RUN: %{dis} fw-noicf.dll | FileCheck --check-prefix=FW-TAKEN %s

## f's and g's jumps go; h's (addend), m's (address held), n's (target inside)
## and p's (target two chunks on) are short; r's (fixed size) keeps five bytes.
# X64:      180001000:   subq $0x28, %rsp
# X64-NEXT: 180001004:   callq 0x180001035
# X64-NEXT: 180001009:   addq $0x28, %rsp
# X64-NEXT: 18000100d:   nopl (%rax)
# X64-NEXT: 180001010:   xorl %eax, %eax
# X64-NEXT: 180001012:   nopw %cs:(%rax,%rax)
# X64-NEXT: 180001020:   jmp 0x180001026
# X64-NEXT: 180001022:   jmp 0x180001024
# X64-NEXT: 180001024:   nop
# X64-NEXT: 180001025:   jmp 0x18000102b
# X64:      18000102b:   nop
# X64-NEXT: 18000102c:   jmp 0x180001035
# X64-NEXT: 18000102e:   retq
# X64-NEXT: 18000102f:   jmp 0x180001035

# GAP:      addq $0x28, %rsp
# GAP-NEXT: 0f 1f 00 nopl (%rax)
# GAP-NEXT: 31 c0 xorl %eax, %eax
# GAP-NEXT: 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00 nopw %cs:(%rax,%rax)

## f ends where its jump began.
# PDATA:      StartAddress: (0x180001000)
# PDATA-NEXT: EndAddress: (0x18000100D)

# FIXUP:      8020 | 0000  | 00001005 | 00001035
# FIXUP-NEXT: 8022 | 0000  | 00001021 | 00001022
# FIXUP-NEXT: 8022 | 0000  | 00001023 | 00001024
# FIXUP-NEXT: 8022 | 0000  | 00001026 | 0000102B
# FIXUP-NEXT: 8022 | 0000  | 0000102D | 00001035
# FIXUP-NEXT: 8002 | 0000  | 00001030 | 00001035
# FIXUP-NOT:  80

# FOREIGN:      180001009:   addq $0x28, %rsp
# FOREIGN-NEXT: 18000100d:   jmp

## g's jump does not end its chunk.
# A64:      180001000:   bl 0x18000101c
# A64-NEXT: 180001004:   nop
# A64-NEXT: 180001008:   nop
# A64-NEXT: 18000100c:   nop
# A64-NEXT: 180001010:   nop
# A64-NEXT: 180001014:   b 0x18000101c
# A64-NEXT: 180001018:   udf #0x0
# A64-NEXT: 18000101c:   ret

# A64-THUNK:      140001000:   tbz w0, #0x0, 0x140001008
# A64-THUNK-NEXT: 140001004:   ret
# A64-THUNK-NEXT: 140001008:   adrp x16, 0x140009000
# A64-THUNK-NEXT: 14000100c:   add x16, x16, #0x14
# A64-THUNK-NEXT: 140001010:   br x16

## The first pass's thunks put near out of reach; they are thrown away and every
## branch retargets before the second.
# RESTART-LOG: Added 9001 thunks with margin 204800 in 2 passes
# RESTART:      140001000:   adr x0, 0x140026000
# RESTART-NEXT: 140001004:   b 0x140009cac
# RESTART-NEXT: 140001008:   b 0x140009cb8
# RESTART:      140009cac:   adrp x16, 0x148003000
# RESTART-NEXT: 140009cb0:   add x16, x16, #0x0
# RESTART-NEXT: 140009cb4:   br x16
# RESTART-NEXT: 140009cb8:   adrp x16, 0x148043000
# RESTART-NEXT: 140009cbc:   add x16, x16, #0x4
# RESTART-NEXT: 140009cc0:   br x16

# FO:      180001000:   jmp 0x180001005 <X>
# FO:      <X>:

## f, whose address is not significant, is g's address, even where its guard
## table lists it.
# FW:      Disassembly of section .text:
# FW-EMPTY:
# FW-NEXT: <g>:
# FW-NEXT: 180001000: retq

## The guard table names the one address once.
# FW-FIDS:      GuardFidTable [
# FW-FIDS-NEXT:   0x180001000{{$}}
# FW-FIDS-NEXT: ]

## f keeps its jump and address where its address-significance table names it,
## and where the link folds nothing.
# FW-TAKEN:      180001000: jmp 0x180001010
# FW-TAKEN:      <g>:
# FW-TAKEN-NEXT: 180001010: retq

#--- x64.s
.macro fn name, sect, align=0
  .section \sect,"xr",discard,\name
  .globl \name
  .p2align \align
\name:
.endm
  .section .text$a,"xr",discard,f
  .globl f
  .seh_proc f
f:
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  callq s
  .seh_startepilogue
  addq $40, %rsp
  .seh_endepilogue
  jmp g
  .seh_endproc

  fn g, .text$b, 4
  xorl %eax, %eax
  jmp h

## A function that is only its jump, with an addend.
  fn h, .text$c, 4
  jmp m+4

## A function that is only its jump, whose address the data holds.
  fn m, .text$d
  jmp n
.Linside = . - 2
  .data
  .quad .Linside

## A target that is not at the start of its chunk.
  fn n, .text$e
  nop
  jmp p

## A target two chunks on.
  .section .text$f,"xr",discard,p
  .long 0
  .globl p
p:
  nop
  jmp s

  fn q, .text$g
  retq

## A section that is not resizable.
  fn r, .text$h
  jmp s
  .org . + 1

  fn s, .text$i
  retq

#--- a64.s
  .section .text$a,"xr",discard,f
  .globl f
f:
  bl h
  b g

  .section .text$b,"xr",discard,g
  .globl g
  .p2align 4
g:
  nop
  b h
  udf #0

  .section .text$c,"xr",discard,h
  .globl h
h:
  ret

## main falls into next and its tbz takes a thunk.
#--- a64-thunk.s
  .section .text$a,"xr"
  .globl main
main:
  tbz w0, #0, far
  b next
  .section .text$b,"xr"
next:
  ret
  .section .text$c,"xr"
  .space 0x8000
  .section .text$d,"xr"
far:
  ret

## near is in main's reach by less than the thunks of its other branches take.
#--- a64-restart.s
  .altmacro
  .macro branch n
  b t\n
  .endm
  .macro target n
  .globl t\n
t\n:
  ret
  .endm

  .section .text$a,"xr",discard,main
  .globl main
main:
  adrp x0, cell
  add x0, x0, :lo12:cell
  b near
  .set i, 0
  .rept 9000
  branch %i
  .set i, i + 1
  .endr
  ret
  .data
cell:
  .quad 0
  .section .far,"bw"
  .zero 0x7fdc000
  .section .tgt,"xr"
  .globl near
near:
  ret
  .zero 0x40000
  .set i, 0
  .rept 9000
  target %i
  .set i, i + 1
  .endr

## A jump to a function override, X, stays: Windows rewrites its site.
#--- fo.s
  .section .text$a,"xr",discard,c1
  .globl c1
c1:
  jmp X
  .section .text$b,"xr",discard,X
  .globl X
X:
  retq
  .include "function-override-x.s"
  .rva c1, X

## A function that is only a jump to the one after it.
#--- fw.s
  .def f; .scl 2; .type 32; .endef
  .def g; .scl 2; .type 32; .endef
  .section .text$a,"xr",discard,f
  .globl f
  .p2align 4
f:
  jmp g
  .section .text$b,"xr",discard,g
  .globl g
  .p2align 4
g:
  retq
  .section .gfids$y,"dr"
  .symidx g
.ifdef LISTED
  .symidx f
.endif
  .globl @feat.00
@feat.00 = 0x800
  .addrsig
.ifdef TAKEN
  .addrsig_sym f
.endif
