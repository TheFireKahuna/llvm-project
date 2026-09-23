# REQUIRES: x86
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/provider.s -o %t/provider.obj
# RUN: lld-link -dll -noentry %t/provider.obj -out:%t/provider.dll -implib:%t/provider.lib -export:entity,DATA -export:name,DATA
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/consumer.s -o %t/consumer.obj
# RUN: lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj %t/provider.lib -out:%t/consumer.dll -export:alias,DATA
# RUN: llvm-readobj --coff-imports --coff-exports --sections %t/consumer.dll | FileCheck %s
# RUN: lld-link -dll -noentry -auto-import -import-slots %t/provider.lib %t/consumer.obj -out:%t/reverse.dll -export:alias,DATA
# RUN: llvm-readobj --coff-imports --coff-exports --sections %t/reverse.dll | FileCheck %s
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj %t/provider.lib -out:%t/writable.dll -section:.rdata,RW 2>&1 | FileCheck %s --check-prefix=PROTECTION
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/opaque.s -o %t/opaque.obj
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj %t/opaque.obj %t/provider.lib -out:%t/opaque.dll 2>&1 | FileCheck %s --check-prefix=OPAQUE
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/version.s -o %t/version.obj
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry %t/version.obj -out:%t/version.dll 2>&1 | FileCheck %s --check-prefix=VERSION
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/invalid.s -o %t/invalid.obj
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry %t/invalid.obj -out:%t/invalid.dll 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/missing.s -o %t/missing.obj
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -force %t/missing.obj -out:%t/missing.dll 2>&1 | FileCheck %s --check-prefix=MISSING
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/legacy.s -o %t/legacy.obj
# RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj %t/legacy.obj %t/provider.lib -out:%t/legacy.dll 2>&1 | FileCheck %s --check-prefix=LEGACY

# Semantic requirements discover imports without a .refptr. Direct references
# in vtables, LSDA and same-address aliases all reach the provider. The losing
# immutable contribution may remain, but must not remain a published identity.
# CHECK-NOT: Name: .llvm.bind
# CHECK: Name: provider.dll
# CHECK: Symbol: entity (0)
# CHECK: Symbol: entity (0)
# CHECK: Symbol: name (0)
# CHECK: Name: alias
# CHECK: ForwardedTo: provider.entity

# PROTECTION: RTTI binding
# PROTECTION-SAME: must reside in read-only, non-executable output
# OPAQUE: is imported, but is referenced as if it were local
# VERSION: unsupported COFF binding or RTTI ABI version
# INVALID: invalid .llvm.bind record at offset 8
# MISSING: canonical binding missing requires a real definition; /force cannot supply its identity
# LEGACY: unversioned RTTI definition _ZTI6Legacy; rebuild for the canonical RTTI ABI

#--- provider.s
.section .rdata,"dr"
.globl entity, name
.p2align 3
entity:
.quad 0, name
name:
.asciz "Entity"

#--- consumer.s
.section .rdata,"dr",discard,entity
.globl entity, alias
.p2align 3
entity:
.quad 0, name
.set alias, entity
.section .rdata,"dr",discard,name
.globl name
name:
.asciz "Entity"
.section .xdata,"dr"
.p2align 3
.quad entity, alias, name
# A local descriptor ensures final section protections are checked even when
# all public descriptors are imported.
.section .rdata,"dr"
local:
.quad 0, 0
.section .llvm.bind,"yn"
.long 1, 2
.symidx entity
.byte 3
.symidx name
.byte 5
.symidx local
.byte 2

#--- opaque.s
.text
.globl opaque
opaque:
# This loads a descriptor's first word, not its address. Replacing it with an
# IAT read would silently change the program. It needs compiler-described
# relaxation or a diagnostic, never a retained local identity.
movq entity(%rip), %rax
retq

#--- version.s
.section .llvm.bind,"yn"
.long 1, 1

#--- invalid.s
.section .llvm.bind,"yn"
.long 1, 2
.long 0xffffffff
.byte 3

#--- missing.s
.section .rdata,"dr"
.quad missing
.section .llvm.bind,"yn"
.long 1, 2
.symidx missing
.byte 3

#--- legacy.s
.section .rdata,"dr",discard,_ZTI6Legacy
.globl _ZTI6Legacy
_ZTI6Legacy:
.quad 0, 0, 0, 0
