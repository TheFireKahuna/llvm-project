# REQUIRES: x86
## The ABI contracts of a joint link. The merge reads the contracts each
## member's inputs provide and gives each member's facts those of the members
## it imports from. A member's final link checks its code's requirements
## against them and, where they match, imports that member's identity, which
## covers them, in place of a witness: a member rebuilt with another layout
## then fails to load beside it. The witnesses stay exported for images linked
## apart, which keep importing them.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a2.s -o a2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium skew.s -o skew.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium c.s -o c.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium helper.s \
# RUN:   -o helper.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium d.s -o d.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium both.s -o both.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium usea.s -o usea.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium used.s -o used.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium inner.s -o inner.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium inner2.s \
# RUN:   -o inner2.obj

# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts

## b.dll imports a.dll's identity and no witness; a.dll exports both, and its
## import library holds the witness but not the identity.
# RUN: llvm-readobj --coff-imports b.dll > b.txt
# RUN: llvm-nm a.lib >> b.txt
# RUN: FileCheck --check-prefix=B %s < b.txt
# B:      Name: a.dll
# B-NEXT: ImportLookupTableRVA:
# B-NEXT: ImportAddressTableRVA:
# B-NEXT: Symbol: __llvm_private$[[ID:[0-9a-f]{16}]] (0)
# B-NEXT: Symbol: g (0)
# B-NEXT: }
# B-NOT:  __llvm_private
# B:      __imp___abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw$a.dll
# B-NOT:  __llvm_private

## An image linked apart against a.dll's import library imports the witness.
# RUN: lld-link -import-slots -dll -noentry b.obj a.lib -out:apart.dll
# RUN: llvm-readobj --coff-imports apart.dll | FileCheck --check-prefix=APART %s
# APART:      Symbol: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw (0)
# APART-NEXT: Symbol: g (2)

## Delay-loaded, the identity is in a.dll's delay-load descriptor, as a witness
## is, which the delay-load helper resolves before the first call.
# RUN: lld-link -import-slots -dll -noentry a.obj -delayload:a.dll \
# RUN:   -out:a.dll -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj helper.obj -delayload:a.dll \
# RUN:   -out:b.dll -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:delay a.jscan b.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj helper.obj -delayload:a.dll \
# RUN:   -out:b.dll -lldjoint:delay/b.dll.jfacts
# RUN: llvm-readobj --coff-imports b.dll | FileCheck --check-prefix=DELAY %s
# DELAY:      DelayImport {
# DELAY-NEXT:   Name: a.dll
# DELAY:          Symbol: __llvm_private${{[0-9a-f]+}} (0)

## a.dll's identity covers the contracts it provides: built with another
## layout of T, it takes another identity, which b.dll's facts then name.
# RUN: lld-link -import-slots -dll -noentry a2.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts2 a.jscan b.jscan
# RUN: llvm-nm facts/b.dll.jfacts | grep __llvm_private > id1.txt
# RUN: llvm-nm facts2/b.dll.jfacts | grep __llvm_private > id2.txt
# RUN: not diff id1.txt id2.txt

## b.dll's code requires the layout a.dll provides; against another, its final
## link fails, as it would against a witness of another contract.
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry skew.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:skew a.jscan b.jscan
# RUN: not lld-link -import-slots -dll -noentry skew.obj -out:b.dll \
# RUN:   -lldjoint:skew/b.dll.jfacts 2>&1 | FileCheck --check-prefix=SKEW %s
# SKEW: error: ABI mismatch for 'T': skew.obj was compiled with a different layout than a.dll exports (needed because skew.obj uses g)

## Two members that one member loads, joined by an import or not, that provide
## different layouts of one type are an error at the merge; two executables,
## which share no process, may differ.
# RUN: lld-link -import-slots -dll -noentry c.obj -out:c.dll \
# RUN:   -lldjointscan:c.jscan
# RUN: not lld-link -lldjointmerge -out:conflict a.jscan c.jscan 2>&1 | \
# RUN:   FileCheck --check-prefix=CONFLICT %s
# CONFLICT: error: ABI contract mismatch for the type with locator Z7E5CzMxeVE: c.dll and a.dll were compiled with different layouts
# RUN: lld-link -import-slots -dll -noentry d.obj -out:d.dll \
# RUN:   -lldjointscan:d.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console both.obj \
# RUN:   -out:both.exe -lldjointscan:both.jscan
# RUN: not lld-link -lldjointmerge -out:conflict a.jscan d.jscan both.jscan \
# RUN:   2>&1 | FileCheck --check-prefix=LOADED %s
# LOADED: error: ABI contract mismatch for the type with locator Z7E5CzMxeVE: a.dll and d.dll were compiled with different layouts
# RUN: lld-link -import-slots -entry:main -subsystem:console usea.obj \
# RUN:   -out:usea.exe -lldjointscan:usea.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console used.obj \
# RUN:   -out:used.exe -lldjointscan:used.jscan
# RUN: lld-link -lldjointmerge -out:apart a.jscan d.jscan usea.jscan \
# RUN:   used.jscan

## Only the contracts of what other images may bind enter a member's identity:
## another layout of a type that only an object with nothing to export uses
## leaves the identity alone.
# RUN: lld-link -import-slots -dll -noentry a.obj inner.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:inner1 a.jscan b.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj inner2.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -lldjointmerge -out:inner2 a.jscan b.jscan
# RUN: llvm-nm inner1/b.dll.jfacts | grep 'T __imp___llvm_private' > in1.txt
# RUN: llvm-nm inner2/b.dll.jfacts | grep 'T __imp___llvm_private' > in2.txt
# RUN: diff in1.txt in2.txt

## The scan reads a bitcode member's contracts from its linker options, before
## LTO.
# RUN: llvm-as a.ll -o a.bc
# RUN: lld-link -import-slots -dll -noentry a.bc -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: not lld-link -lldjointmerge -out:conflict a.jscan c.jscan 2>&1 | \
# RUN:   FileCheck --check-prefix=CONFLICT %s

#--- a.s
  .text
  .globl g
g:
  retq
  .section .drectve,"yn"
  .ascii " -export:g /abiprovide:Z7E5CzMxeVEAAECAwQFBgcICQoLDA0ODw"

#--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

!llvm.linker.options = !{!0, !1}
!0 = !{!"/export:g"}
!1 = !{!"/abiprovide:Z7E5CzMxeVEAAECAwQFBgcICQoLDA0ODw"}

define void @g() {
  ret void
}

#--- a2.s
  .text
  .globl g
g:
  retq
  .section .drectve,"yn"
  .ascii " -export:g /abiprovide:Z7E5CzMxeVE8PHy8_T19vf4-fr7_P3-_w"

#--- b.s
  .text
  .globl bf
bf:
  .linkcontract bf, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
  callq *__imp_g(%rip)
  retq
  .section .drectve,"yn"
  .ascii " -export:bf"

#--- skew.s
  .text
  .globl bf
bf:
  .linkcontract bf, __imp_g, type, "1T", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
  callq *__imp_g(%rip)
  retq
  .section .drectve,"yn"
  .ascii " -export:bf"

#--- c.s
  .text
  .globl cf
cf:
  callq *__imp_g(%rip)
  retq
  .section .drectve,"yn"
  .ascii " -export:cf /abiprovide:Z7E5CzMxeVE8PHy8_T19vf4-fr7_P3-_w"

#--- d.s
  .text
  .globl h
h:
  retq
  .section .drectve,"yn"
  .ascii " -export:h /abiprovide:Z7E5CzMxeVE8PHy8_T19vf4-fr7_P3-_w"

#--- both.s
  .text
  .globl main
main:
  callq *__imp_g(%rip)
  callq *__imp_h(%rip)
  retq

#--- usea.s
  .text
  .globl main
main:
  callq *__imp_g(%rip)
  retq

#--- used.s
  .text
  .globl main
main:
  callq *__imp_h(%rip)
  retq

#--- inner.s
  .text
  .globl i
i:
  retq
  .section .drectve,"yn"
  .ascii " /abiprovide:PwFGDH9_sEQEBESExQVFhcYGRobHB0eHw"

#--- inner2.s
  .text
  .globl i
i:
  retq
  .section .drectve,"yn"
  .ascii " /abiprovide:PwFGDH9_sEQ8PHy8_T19vf4-fr7_P3-_w"

#--- helper.s
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq
