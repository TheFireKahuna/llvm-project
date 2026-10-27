# REQUIRES: x86
# RUN: %python %p/Inputs/def-many.py 65535 > %t-65535.def
# RUN: llvm-mc -triple x86_64-win32 %s -filetype=obj -o %t.obj
# RUN: lld-link -dll -noentry %t.obj -out:%t.dll -def:%t-65535.def
# RUN: echo '.globl g; g: ret' | llvm-mc -triple x86_64-win32 -filetype=obj -o %t-g.obj
## The files are listed by how many of the exported symbols they define.
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry %t-g.obj %t.obj -out:%t.dll -def:%t-65535.def -export:g 2>&1 | FileCheck %s

# CHECK:      error: too many exported symbols (got 65536, max 65535)
# CHECK-NEXT: >>> 65535 defined in {{.*}}export-limit.s.tmp.obj
# CHECK-NEXT: >>> 1 defined in {{.*}}-g.obj
# CHECK-NOT:  >>>

        .text
        .globl f
f:
        ret
