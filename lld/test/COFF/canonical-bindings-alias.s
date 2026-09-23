# REQUIRES: x86
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/provider.s -o %t/provider.obj
# RUN: lld-link -dll -noentry %t/provider.obj -out:%t/provider.dll -implib:%t/provider.lib -export:entity,DATA
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/consumer.s -o %t/consumer.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/weak.s -o %t/weak.obj
# RUN: lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj %t/weak.obj %t/provider.lib -out:%t/weak.dll -export:a,DATA
# RUN: llvm-readobj --coff-imports --coff-exports %t/weak.dll | FileCheck %s
# RUN: lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj %t/provider.lib -out:%t/alternate.dll -export:a,DATA -alternatename:a=b -alternatename:b=entity
# RUN: llvm-readobj --coff-imports --coff-exports %t/alternate.dll | FileCheck %s
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/strong.s -o %t/strong.obj
# RUN: lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj %t/weak.obj %t/strong.obj %t/provider.lib -out:%t/strong.dll -export:a,DATA
# RUN: llvm-readobj --coff-exports %t/strong.dll | FileCheck %s --check-prefix=STRONG

# Conditional weak aliases must publish the chosen provider, not dormant local
# RTTI. Alternate-name chains need a fixed point even without archive progress.
# A strong definition still overrides its fallback under ordinary COFF rules.
# CHECK: Name: provider.dll
# CHECK: Symbol: entity (0)
# CHECK: Name: a
# CHECK: ForwardedTo: provider.entity
# STRONG: Name: a
# STRONG-NOT: ForwardedTo:

#--- provider.s
.section .rdata,"dr"
.globl entity
entity:
.quad 0, 0

#--- consumer.s
.section .rdata,"dr",discard,entity
.globl entity
entity:
.quad 0, 0
.section .rdata,"dr"
.quad a, b, entity
.section .llvm.bind,"yn"
.long 1, 2
.symidx a
.byte 3
.symidx entity
.byte 3

#--- weak.s
.weak a, b
.set a, b
.set b, entity

#--- strong.s
.section .rdata,"dr"
.globl a
a:
.quad 0, 0
