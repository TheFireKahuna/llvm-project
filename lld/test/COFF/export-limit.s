# REQUIRES: x86
# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: %python %p/Inputs/def-many.py 65535 > 65535.def
# RUN: %python %p/Inputs/def-many.py 65536 > 65536.def
# RUN: llvm-mc -triple x86_64-win32 f.s -filetype=obj -o f.obj
# RUN: llvm-mc -triple x86_64-win32 g.s -filetype=obj -o g.obj
# RUN: lld-link -dll -noentry f.obj -out:out.dll -def:65535.def
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry f.obj -out:out.dll \
# RUN:   -def:65536.def 2>&1 | FileCheck %s

# CHECK:      error: too many exported symbols (got 65536, max 65535)
# CHECK-NEXT: >>> 65536 defined in f.obj
# CHECK-NOT:  >>>

## The files are listed by how many of the exported symbols they define.
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry g.obj f.obj -out:out.dll \
# RUN:   -def:65535.def -export:g 2>&1 | FileCheck --check-prefix=TWO %s

# TWO:      error: too many exported symbols (got 65536, max 65535)
# TWO-NEXT: >>> 65535 defined in f.obj
# TWO-NEXT: >>> 1 defined in g.obj

#--- f.s
        .text
        .globl f
f:
        ret

#--- g.s
        .text
        .globl g
g:
        ret
