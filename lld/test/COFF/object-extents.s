# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %s -o %t.obj
# RUN: lld-link /dll /noentry /export:data /out:%t.dll %t.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym size=17 %s -o %t.large.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /out:%t.dll %t.large.obj 2>&1 | FileCheck %s --check-prefix=LARGE
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym size=0 %s -o %t.zero.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /out:%t.dll %t.zero.obj 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym overlong=1 %s -o %t.overlong.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /out:%t.dll %t.overlong.obj 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym conflict=1 %s -o %t.conflict.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /out:%t.dll %t.conflict.obj 2>&1 | FileCheck %s --check-prefix=CONFLICT
# LARGE: object extent exceeds data storage
# INVALID: invalid object extent
# CONFLICT: conflicting object extents

.section .rdata,"dr"
.globl data
data:
.quad 11, 22

.section .llvm.extent,"i"
.long 1
.symidx data
.ifdef overlong
.byte 0x90, 0
.else
.ifndef size
.set size, 16
.endif
.uleb128 size
.endif
.ifdef conflict
.symidx data
.uleb128 8
.endif
