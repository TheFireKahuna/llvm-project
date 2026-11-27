# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-pc-win32 %s -o %t1.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-pc-win32 %S/Inputs/icf-safe.s -o %t2.obj
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf %t1.obj %t2.obj 2>&1 | FileCheck %s
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf /export:g3 /export:g4 %t1.obj %t2.obj 2>&1 | FileCheck --check-prefix=EXPORT %s
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,safeicf %t1.obj %t2.obj 2>&1 | FileCheck %s --check-prefix=SAFEICF

## In a link that holds an object for Windows Itanium or NT-POSIX, /opt:icf
## keeps the code of a significant address distinct too: what a fork object's
## address-significance table lists, what a foreign object's address-
## significance or guard table lists, and all of an object with neither.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %s -o %t3.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium --defsym=GFIDS=1 %s -o %t4.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium --defsym=NOTABLE=1 %s -o %t5.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-pc-win32 --defsym=GFIDS=1 %s -o %t6.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-pc-win32 --defsym=NOTABLE=1 %s -o %t7.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/icf-safe.s -o %t8.obj
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf %t3.obj 2>&1 | FileCheck %s --check-prefix=SAFEICF
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf %t4.obj 2>&1 | FileCheck %s --check-prefix=SAFEICF
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf %t5.obj 2>&1 | FileCheck %s --check-prefix=NONE
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf %t1.obj %t8.obj 2>&1 | FileCheck %s --check-prefix=SAFEICF
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf %t6.obj %t8.obj 2>&1 | FileCheck %s --check-prefix=GFIDS
# RUN: lld-link /dll /noentry /out:%t.dll /verbose /opt:noref,icf %t7.obj %t8.obj 2>&1 | FileCheck %s --check-prefix=NONE

# CHECK-NOT:  Selected
# CHECK:      Selected f1
# CHECK-NEXT:   Removed f2
# CHECK-NEXT:   Removed f3
# CHECK-NEXT:   Removed f4
# CHECK-NEXT: Selected g3
# CHECK-NEXT:   Removed g4
# CHECK-NOT:  {{.}}

# EXPORT-NOT: Selected g3
# EXPORT-NOT: Selected g4

# SAFEICF-NOT:  Selected
# SAFEICF:      Selected f3
# SAFEICF-NEXT:   Removed f4
# SAFEICF-NEXT: Selected g3
# SAFEICF-NEXT:   Removed g4
# SAFEICF-NOT:  {{.}}

# GFIDS-NOT:  Selected
# GFIDS:      Selected g3
# GFIDS-NEXT:   Removed g4
# GFIDS-NOT:  {{.}}

# NONE-NOT: Selected

.section .rdata,"dr",one_only,g1
.globl g1
g1:
.byte 1

.section .rdata,"dr",one_only,g2
.globl g2
g2:
.byte 1

.section .rdata,"dr",one_only,g3
.globl g3
g3:
.byte 2

.section .rdata,"dr",one_only,g4
.globl g4
g4:
.byte 2

.section .text,"xr",one_only,f1
.globl f1
f1:
 nop

.section .text,"xr",one_only,f2
.globl f2
f2:
 nop

.section .text,"xr",one_only,f3
.globl f3
f3:
 nop

.section .text,"xr",one_only,f4
.globl f4
f4:
 nop

.ifdef GFIDS
.section .gfids$y,"dr"
.symidx f3
.endif

.ifndef NOTABLE
.addrsig
.addrsig_sym g1
.addrsig_sym g2
.addrsig_sym f1
.addrsig_sym f2
.endif
