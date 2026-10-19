# REQUIRES: x86
## A scan writes the member's import library where none is there or where a
## scan wrote it, which a member of the library marks, so that the scans of the
## members that link it see its exports as their final links will; it never
## writes over the one a final link wrote.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a1.s -o a1.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a2.s -o a2.obj

# RUN: lld-link -import-slots -dll -noentry a1.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: llvm-nm a.lib | FileCheck --check-prefix=SCAN1 %s
# SCAN1-NOT:  __imp_ag
# SCAN1:      __llvm_jointscan$a.dll
# SCAN1-NOT:  __imp_ag

## The member now exports more: its next scan writes the library again.
# RUN: lld-link -import-slots -dll -noentry a2.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: llvm-nm a.lib | FileCheck --check-prefix=SCAN2 %s
# SCAN2: __imp_ag
# SCAN2: __llvm_jointscan$a.dll

## Once the final link has written it, a scan leaves it alone.
# RUN: lld-link -lldjointmerge -out:facts a.jscan
# RUN: lld-link -import-slots -dll -noentry a2.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: llvm-nm a.lib | FileCheck --check-prefix=FINAL %s
# RUN: cp a.lib final.lib
# RUN: lld-link -import-slots -dll -noentry a1.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: cmp a.lib final.lib
# FINAL:     __imp_ag
# FINAL-NOT: __llvm_jointscan

#--- a1.s
  .text
  .globl af
af:
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- a2.s
  .text
  .globl af, ag
af:
  retq
ag:
  retq

  .section .drectve,"yni"
  .ascii " -export:af -export:ag"
