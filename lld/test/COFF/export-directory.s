# REQUIRES: x86

## The loader reads an exported address inside the export directory as a
## forwarder string. An export that ends where the directory would start gets
## a gap of the directory's alignment between them; an export inside the
## directory is an error.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %s -o %t.obj
# RUN: lld-link -dll -noentry -base:0x10000000 -out:%t.dll %t.obj \
# RUN:   -export:array_end,DATA
# RUN: llvm-readobj --file-headers --coff-exports %t.dll | FileCheck %s
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -base:0x10000000 \
# RUN:   -out:%t.dll %t.obj -export:array_end,DATA -export:inside,DATA 2>&1 \
# RUN:   | FileCheck --check-prefix=ERR %s

# CHECK:      ExportTableRVA: 0x1011
# CHECK:      Name: array_end
# CHECK-NEXT: RVA: 0x1010

# ERR: error: exported symbol inside is inside the export directory

  .section .rdata,"dr"
  .p2align 4
  .globl array_end
  .quad 1, 2
array_end:

  .globl inside
inside = 0x10001014
