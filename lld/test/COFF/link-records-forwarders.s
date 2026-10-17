# REQUIRES: x86
## A forwarder, a function that only jumps through the import pointer of an
## equivalent function, is replaced by that function where the image defines
## it: every reference to the forwarder reaches the function, and the
## forwarder is discarded. A forwarder to an imported function stays.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium fwd.s -o fwd.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium base.s -o base.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64

# RUN: lld-link -entry:main -subsystem:console fwd.obj base.obj -out:local.exe \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d local.exe | FileCheck --check-prefix=LOCAL %s
# RUN: llvm-objdump -s -j .rdata local.exe | FileCheck --check-prefix=WORD %s
# LOG: Replacing forwarder fwd in fwd.obj by base
# LOCAL:      <.text>:
# LOCAL-NEXT: 140001000: b8 2a 00 00 00 movl $0x2a, %eax
# LOCAL-NOT:  jmpq
# WORD: 140002000 00100040 01000000

# RUN: lld-link -entry:main -subsystem:console fwd.obj lib.lib -out:imp.exe \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=KEEP %s
# RUN: llvm-objdump -d imp.exe | FileCheck --check-prefix=IMP %s
# KEEP-NOT: Replacing forwarder
# IMP: jmpq *{{.*}}(%rip)

#--- lib.def
LIBRARY lib.dll
EXPORTS
  base

#--- fwd.s
  .section .text,"xr",discard,fwd
  .globl fwd
fwd:
  .linkforwarder fwd, __imp_base
  rex64 jmpq *__imp_base(%rip)

  .section .rdata,"dr"
  .globl main
main:
  .quad fwd

#--- base.s
  .text
  .globl base
base:
  movl $42, %eax
  retq
