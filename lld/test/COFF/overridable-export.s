# REQUIRES: x86
## Only the executable replaces an overridable default for every image of the
## program. A DLL linked under -import-slots that exports a strong definition
## of a function that an input marks as an overridable default makes an offer:
## it exports the body under the offer name, so no importer binds to it as the
## ABI symbol, and an executable that links the DLL selects the offer. The
## runtime's own default is a weak definition and is exported under its ABI
## name, as is a definition the DLL keeps to itself. Strength comes from the
## defining symbol, before and after LTO.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium strong.s -o strong.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium weak.s -o weak.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium hidden.s -o hidden.obj
# RUN: llvm-as strong.ll -o strong.bc
# RUN: llvm-as weak.ll -o weak.bc

## A strong definition is offered: the DLL exports the body under the offer
## name, not under the ABI name, so no importer binds to it as operator new.
# RUN: lld-link -import-slots -dll -noentry strong.obj -out:strong.dll
# RUN: llvm-readobj --coff-exports strong.dll | FileCheck --check-prefix=OFFER %s
# RUN: lld-link -import-slots -dll -noentry strong.bc -out:strong-bc.dll
# RUN: llvm-readobj --coff-exports strong-bc.dll | FileCheck --check-prefix=OFFER %s
# OFFER:      Name: __ovr_offer$_Znwy
# OFFER-NOT:  Name: _Znwy

## The runtime's own weak default is exported under its ABI name.
# RUN: lld-link -import-slots -dll -noentry weak.obj -out:weak.dll
# RUN: llvm-readobj --coff-exports weak.dll | FileCheck --check-prefix=EXPORTS %s
# RUN: lld-link -import-slots -dll -noentry weak.bc -out:weak-bc.dll
# RUN: llvm-readobj --coff-exports weak-bc.dll | FileCheck --check-prefix=EXPORTS %s
# EXPORTS:     Name: _Znwy
# EXPORTS-NOT: Name: __ovr_offer$_Znwy

## A definition the DLL keeps to itself exports nothing, and makes no offer.
# RUN: lld-link -import-slots -dll -noentry hidden.obj -out:hidden.dll
# RUN: llvm-readobj --coff-exports hidden.dll | FileCheck --check-prefix=NONE %s
# NONE-NOT: Name:

## Without -import-slots the DLL has no offer form; the strong export keeps its
## ABI name, as link.exe would export it.
# RUN: lld-link -dll -noentry strong.obj -out:plain.dll
# RUN: llvm-readobj --coff-exports plain.dll | FileCheck --check-prefix=EXPORTS %s

#--- strong.s
  .text
  .globl _Znwy
_Znwy:
  xorl %eax, %eax
  retq

  .section .drectve,"yni"
  .ascii " /OVERRIDABLE:_Znwy /EXPORT:_Znwy"

#--- weak.s
  .text
  .weak _Znwy
_Znwy:
  xorl %eax, %eax
  retq

  .section .drectve,"yni"
  .ascii " /OVERRIDABLE:_Znwy /EXPORT:_Znwy"

#--- hidden.s
  .text
  .globl _Znwy
_Znwy:
  xorl %eax, %eax
  retq

  .section .drectve,"yni"
  .ascii " /OVERRIDABLE:_Znwy"

#--- strong.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define dllexport ptr @_Znwy(i64 %n) {
  ret ptr null
}

!llvm.linker.options = !{!0}
!0 = !{!"/OVERRIDABLE:_Znwy"}

#--- weak.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define weak dllexport ptr @_Znwy(i64 %n) {
  ret ptr null
}

!llvm.linker.options = !{!0}
!0 = !{!"/OVERRIDABLE:_Znwy"}
