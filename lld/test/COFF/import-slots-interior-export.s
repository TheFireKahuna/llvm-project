# REQUIRES: x86
## Under -import-slots, an image that exports a definition by a module
## definition file or /export also exports the names of the addresses inside
## it, X$soK and X$apK, under its export name, as data and with its privacy.
## An export from the objects' directives carries its names with it already.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium directive.s \
# RUN:   -o directive.obj

# RUN: lld-link -dll -noentry -import-slots -def:lib.def -out:def.dll lib.obj
# RUN: llvm-readobj --coff-exports def.dll | FileCheck --check-prefix=DEF %s
# RUN: lld-link -dll -noentry -import-slots -export:obj,data \
# RUN:   -export:renamed=_ZTV1D,data -out:export.dll lib.obj
# RUN: llvm-readobj --coff-exports export.dll | \
# RUN:   FileCheck --check-prefix=EXPORT %s
# RUN: lld-link -dll -noentry -import-slots -out:directive.dll directive.obj
# RUN: llvm-readobj --coff-exports directive.dll | \
# RUN:   FileCheck --check-prefix=DIRECTIVE %s
# RUN: lld-link -dll -noentry -export:obj,data -out:noslots.dll lib.obj
# RUN: llvm-readobj --coff-exports noslots.dll | \
# RUN:   FileCheck --check-prefix=NOSLOTS %s

# DEF:     Name: _ZTV1D
# DEF:     Name: _ZTV1D$ap16
# DEF:     Name: obj
# DEF:     Name: obj$so16
# DEF:     Name: obj$so8
# DEF-NOT: Name:

# EXPORT:     Name: obj
# EXPORT:     Name: obj$so16
# EXPORT:     Name: obj$so8
# EXPORT:     Name: renamed
# EXPORT:     Name: renamed$ap16
# EXPORT-NOT: Name:

# DIRECTIVE:     Name: obj
# DIRECTIVE-NOT: Name:

# NOSLOTS:     Name: obj
# NOSLOTS-NOT: Name:

#--- lib.def
LIBRARY def.dll
EXPORTS
  obj DATA
  _ZTV1D DATA

#--- lib.s
  .data
  .p2align 3
  .globl obj
obj:
  .quad 0, 0, 0
  .globl "obj$so8"
  .set "obj$so8", obj+8
  .globl "obj$so16"
  .set "obj$so16", obj+16

  .section .rdata,"dr"
  .p2align 3
  .globl _ZTV1D
_ZTV1D:
  .quad 0, 0, 0
  .globl "_ZTV1D$ap16"
  .set "_ZTV1D$ap16", _ZTV1D+16

#--- directive.s
  .data
  .p2align 3
  .globl obj
obj:
  .quad 0, 0
  .globl "obj$so8"
  .set "obj$so8", obj+8

  .section .drectve,"yni"
  .ascii " -export:obj,data"
