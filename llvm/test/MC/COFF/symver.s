# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ok.s -o ok.obj
# RUN: llvm-readobj --symbols ok.obj | FileCheck %s
# RUN: %if aarch64-registered-target %{ llvm-mc -triple=aarch64-windows-gnu ok.s | FileCheck --check-prefix=ASM %s %}
# RUN: not llvm-mc -filetype=obj -triple=x86_64-windows-itanium bad.s \
# RUN:   -o /dev/null 2>&1 | FileCheck --check-prefix=ERR %s
# RUN: not llvm-mc -filetype=obj -triple=x86_64-windows-itanium undefined.s \
# RUN:   -o /dev/null 2>&1 | FileCheck --check-prefix=UNDEF %s

## COFF has no symbol versions: .symver defines the versioned name as an
## external alias of the symbol, whatever its binding, and "@@@" is the
## default version. 'remove' and the version of an undefined symbol, which
## COFF cannot express, are errors.

# CHECK:      Name: f@V1
# CHECK-NEXT: Value: 0
# CHECK-NEXT: Section: .text (
# CHECK:      StorageClass: External
# CHECK:      Name: f@@V2
# CHECK-NEXT: Value: 1
# CHECK-NEXT: Section: .text (
# CHECK:      StorageClass: External
# CHECK:      Name: v@@V2
# CHECK-NEXT: Value: 0
# CHECK-NEXT: Section: .data (
# CHECK:      StorageClass: External

# ASM: .symver f_v1, f@V1
# ASM: .symver f_v2, f@@V2
# ASM: .symver v, v@@@V2

# ERR:      bad.s:1:16: error: expected a '@' in the name
# ERR:      bad.s:2:16: error: 'remove' is not supported
# ERR:      bad.s:3:11: error: expected a comma
# UNDEF:    error: .symver of 'undefined', which this object does not define: a COFF reference cannot name a version

#--- ok.s
.symver f_v1, f@V1
.symver f_v2, f@@V2
.symver v, v@@@V2
.text
f_v1:
  ret
.globl f_v2
f_v2:
  ret
.data
v:
  .long 0

#--- bad.s
.symver f, f_V1
.symver f, f@V1, remove
.symver f f@V1
f:
  ret

#--- undefined.s
.symver undefined, g@V1
