# REQUIRES: aarch64

## A pass that throws the last pass's thunks away judges each branch by the
## target its object names, and a branch to its own chunk by its distance
## alone, which no thunk changes. A narrow branch is judged by its distance
## until a layout leaves it out of reach, and takes no thunk it cannot reach.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir

## The thunks of the first pass move two of the targets within its margin out
## of reach; the second pass gives those two thunks too, and the rest none.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows multipass.s -o multipass.obj
# RUN: lld-link -entry:main -subsystem:console multipass.obj -out:multipass.exe \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=MULTIPASS %s
# MULTIPASS: Added 20 thunks with margin 204800 in 2 passes

## A foreign tbz and cbz that reach take no thunk in an image that needs one,
## the tbz at the start of a chunk over 32 KB too; one that neither its target
## nor a thunk after its section would reach is an error.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows narrow.s -o narrow.obj
# RUN: lld-link -entry:main -subsystem:console narrow.obj -out:narrow.exe \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=NARROW-LOG %s
# RUN: llvm-objdump -d --no-show-raw-insn narrow.exe | \
# RUN:   FileCheck --check-prefix=NARROW %s
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows --defsym BIG=1 narrow.s \
# RUN:   -o big.obj
# RUN: lld-link -entry:main -subsystem:console big.obj -out:big.exe -verbose \
# RUN:   2>&1 | FileCheck --check-prefix=NARROW-LOG %s
# RUN: llvm-objdump -d --no-show-raw-insn big.exe | \
# RUN:   FileCheck --check-prefix=NARROW %s
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows --defsym BIG=1 \
# RUN:   --defsym BAD=1 narrow.s -o bad.obj
# RUN: not lld-link -entry:main -subsystem:console bad.obj -out:bad.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD %s
# NARROW-LOG: Added 1 thunks with margin 102400 in 1 passes
# NARROW:      <.text>:
# NARROW-NEXT: tbz w0, #0x0, 0x140001008
# NARROW-NEXT: ret
# NARROW-NEXT: adrp x16
# NARROW-NEXT: add x16, x16
# NARROW-NEXT: br x16
# NARROW-NEXT: 140001014: ret
# NARROW-NEXT: tbz w1, #0x0, 0x140001014
# NARROW-NEXT: cbz x1, 0x140001014
# BAD: error: bad.obj: .text$c+0xc: a branch to far is out of range, and so is a range extension thunk after the section

## The tbz over the resized pair reaches .Lin, 32,760 bytes on, by itself, and
## only the tbz to far takes a thunk.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium own.s -o own.obj
# RUN: lld-link -entry:main -subsystem:console own.obj -out:own.exe \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=OWN-LOG %s
# RUN: llvm-objdump -d --no-show-raw-insn own.exe | FileCheck --check-prefix=OWN %s
# OWN-LOG: Added 1 thunks with margin 102400 in 1 passes
# OWN:      <.text>:
# OWN-NEXT: tbz w0, #0x0, 0x140008ff8 <.text+0x7ff8>
# OWN-NEXT: adr x8,

#--- multipass.s
  .globl main
main:
  b.ne func01
  ret

.irp i, 01, 02, 03, 04, 05, 06, 07, 08, 09, 10, 11, 12, 13, 14, 15, 16, 17, 18
  .section .text$\i\()a, "xr"
  .balign 8192
func\i:
  b.ne far_func\i
  b.ne func_within_margin\i
  ## With a thunk after it, the next section moves on by 8192 bytes.
  .space 8192 - 8 - 4

  .section .text$\i\()b, "xr"
  .balign 8192
align\i:
  nop
.endr

  .section .text$999, "xr"
tail:
  .space 0x100000 - 100*1024 - 18*8192*2
.irp i, 01, 02, 03, 04, 05, 06, 07, 08, 09, 10, 11, 12, 13, 14, 15, 16, 17, 18
func_within_margin\i:
  nop
.endr
  .space 0x100000
.irp i, 01, 02, 03, 04, 05, 06, 07, 08, 09, 10, 11, 12, 13, 14, 15, 16, 17, 18
far_func\i:
  nop
.endr
  ret

#--- narrow.s
  .globl main
main:
  tbz w0, #0, far
  ret
  .section .text$b,"xr"
near:
  ret
  .section .text$c,"xr"
  tbz w1, #0, near
  cbz x1, near
  ret
.ifdef BAD
  tbz w2, #0, far
.endif
.ifdef BIG
  .space 0x9000
.endif
  .section .text$d,"xr"
  .space 0x9000
far:
  ret

#--- own.s
  .globl main
main:
  tbz w0, #0, .Lin
  adrp x8, data
  add x8, x8, :lo12:data
  .space 0x7ff0
.Lin:
  tbz w1, #0, far
  ret
  .section .text$b,"xr"
  .space 0x8000
far:
  ret
  .data
data:
  .word 0
