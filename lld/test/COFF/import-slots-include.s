# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium %s -o %t.obj

## An object whose static data holds an imported address includes
## __llvm_import_slots_v1, which the linker defines only under -import-slots,
## so that a link that would not have the loader write the address fails.
# RUN: lld-link -import-slots -entry:main -subsystem:console %t.obj \
# RUN:   -out:%t.exe
# RUN: not lld-link -entry:main -subsystem:console %t.obj -out:%t.exe 2>&1 \
# RUN:   | FileCheck %s
# CHECK: error: <root>: undefined symbol: __llvm_import_slots_v1

  .globl main
main:
  ret

  .section .drectve,"yn"
  .ascii " /INCLUDE:__llvm_import_slots_v1"
