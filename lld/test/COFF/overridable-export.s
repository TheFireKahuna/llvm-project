# REQUIRES: x86
## Only the executable replaces an overridable default for every image of the
## program. A DLL linked under -import-slots that exports a strong definition
## of a function that an input marks as an overridable default is an error.
## The runtime's own default is a weak definition and is exported cleanly, as
## is a definition that the DLL keeps to itself. Strength comes from the
## defining symbol, before and after LTO.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium strong.s -o strong.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium weak.s -o weak.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium hidden.s -o hidden.obj
# RUN: llvm-as strong.ll -o strong.bc
# RUN: llvm-as weak.ll -o weak.bc

# RUN: not lld-link -import-slots -dll -noentry strong.obj -out:strong.dll 2>&1 \
# RUN:   | FileCheck --check-prefix=STRONG %s
# RUN: not lld-link -import-slots -dll -noentry strong.bc -out:strong.dll 2>&1 \
# RUN:   | FileCheck --check-prefix=STRONG-BC %s
# STRONG: error: strong.obj: the DLL exports operator new(unsigned long long), which replaces an overridable default for this DLL and its importers only; replace it in the executable, which replaces it for every image, or give the definition hidden visibility
# STRONG-BC: error: {{.*}}: the DLL exports operator new(unsigned long long), which replaces an overridable default

# RUN: lld-link -import-slots -dll -noentry weak.obj -out:weak.dll
# RUN: llvm-readobj --coff-exports weak.dll | FileCheck --check-prefix=EXPORTS %s
# RUN: lld-link -import-slots -dll -noentry weak.bc -out:weak-bc.dll
# RUN: llvm-readobj --coff-exports weak-bc.dll | FileCheck --check-prefix=EXPORTS %s
# EXPORTS: Name: _Znwy

# RUN: lld-link -import-slots -dll -noentry hidden.obj -out:hidden.dll
# RUN: lld-link -dll -noentry strong.obj -out:strong.dll
# RUN: lld-link -import-slots -entry:main -subsystem:console strong.obj \
# RUN:   -out:strong.exe

#--- strong.s
  .text
  .globl _Znwy
_Znwy:
  xorl %eax, %eax
  retq

  .globl main
main:
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
