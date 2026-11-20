# REQUIRES: x86
# RUN: llvm-mc -triple=x86_64-unknown-windows-itanium -filetype=obj %s -o %t.obj
# RUN: not lld-link /entry:f /subsystem:console /out:%t.exe %t.obj 2>&1 | \
# RUN:   FileCheck %s

## An object for Windows Itanium has a label at each point of resizable code
## that something depends on. A reference is reported in the function that
## holds it, not at the label before it.

# CHECK:      error: undefined symbol: g
# CHECK-NEXT: >>> referenced by {{.*}}.obj:(f)

  .text
  .globl f
f:
  je .Lnext
.Lnext:
  callq g
  retq
