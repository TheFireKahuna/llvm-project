# REQUIRES: x86, aarch64
## The forms of a delay-load guard beyond those of delayimports-address.s:
## on AArch64 a read of a delay-bound section and its address behind the
## gate of every DLL its words hold, a foreign object's read of an import's
## entry, the far forms and the long unwind record; on x86-64 a lea behind
## REX2, the address inside a function, an access of unstated width, and the
## functions a foreign object observes without addrsig.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -dll -noentry -guard:cf -include:main \
# DEFINE:   -call-graph-profile-sort:none -delayload:d.dll -delayload:e.dll

# RUN: lld-link -machine:arm64 -def:d.def -out:d-a64.lib
# RUN: lld-link -machine:arm64 -def:e.def -out:e-a64.lib
# RUN: %{mc} -triple=aarch64-windows-itanium reads-a64.s -o reads-a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium -I %S/Inputs rt.s -o rt-a64.obj
# RUN: %{mc} -triple=aarch64-windows-msvc foreign-a64.s -o foreign-a64.obj
# RUN: %{link} d-a64.lib e-a64.lib reads-a64.obj rt-a64.obj foreign-a64.obj \
# RUN:   -export:other -export:proto -export:long -export:wide -export:deep \
# RUN:   -out:a64.dll
# RUN: llvm-objdump -d a64.dll > a64.txt
# RUN: llvm-readobj --unwind a64.dll >> a64.txt
# RUN: FileCheck --check-prefix=A64 %s < a64.txt

## A read of t1, which holds f and e, branches to a guard that tests d.dll's
## gate, then e.dll's, calling the ensure stub of each that is unbound, and
## then redoes the adrp; its load stays in place.
# A64:      <.text>:
# A64-NEXT: [[#%x,SITE:]]: {{.*}} b 0x[[#%x,GUARD:]]
# A64-NEXT: [[#%x,BACK:]]: {{.*}} ldr x0, [x8, #0x8]
## t1's address in a pair the link may resize is tested inline, gate by gate.
# A64-NEXT: adrp x9, 0x[[#%x,GATES:]]
# A64-NEXT: ldr x9, [x9, #0x[[#%x,DGATE:]]]
# A64-NEXT: tbnz w9, #0x0, 0x[[#%x,IDCALL:]]
# A64-NEXT: adrp x9, 0x[[#GATES]]
# A64-NEXT: ldr x9, [x9, #0x[[#%x,EGATE:]]]
# A64-NEXT: tbnz w9, #0x0, 0x[[#%x,IECALL:]]
# A64-NEXT: adrp x9, 0x[[#%x,T1:]]
# A64-NEXT: add x9, x9, #0x0
## A store, and a load that misses every word of s, are not guarded; a load
## of s's word is.
# A64-NEXT: adrp x10
# A64-NEXT: str x1, [x10]
# A64-NEXT: adrp x11, 0x[[#T1]]
# A64-NEXT: ldr w2, [x11, #0x20]
# A64-NEXT: b 0x[[#%x,SGUARD:]]
# A64-NEXT: ldr x3, [x12, #0x18]
## So is the read of a local import's pointer to t2, which holds g.
# A64-NEXT: adrp x13, 0x[[#GATES]]
# A64-NEXT: ldr x13, [x13, #0x[[#DGATE]]]
# A64-NEXT: tbnz w13, #0x0
# A64-NEXT: adrp x13
# A64-NEXT: ldr x13
# A64-NEXT: bl
# A64-NEXT: ret
# A64-NEXT: [[#GUARD]]: {{.*}} adrp x8, 0x[[#GATES]]
# A64-NEXT: ldr x8, [x8, #0x[[#DGATE]]]
# A64-NEXT: tbz w8, #0x0, 0x[[#%x,NEXT:]]
# A64-NEXT: adr x8, 0x[[#SITE]]
# A64-NEXT: stp x30, x8, [sp, #-0x10]!
# A64-NEXT: bl 0x[[#%x,DENSURE:]]
# A64-NEXT: ldr x30, [sp], #0x10
# A64-NEXT: [[#NEXT]]: {{.*}} adrp x8, 0x[[#GATES]]
# A64-NEXT: ldr x8, [x8, #0x[[#EGATE]]]
# A64-NEXT: tbz w8, #0x0, 0x[[#%x,TAIL:]]
# A64-NEXT: adr x8, 0x[[#SITE]]
# A64-NEXT: stp x30, x8, [sp, #-0x10]!
# A64-NEXT: bl 0x[[#%x,EENSURE:]]
# A64-NEXT: ldr x30, [sp], #0x10
# A64-NEXT: [[#TAIL]]: {{.*}} adrp x8, 0x[[#T1]]
# A64-NEXT: b 0x[[#BACK]]
## The inline site enters each gate's block at its call.
# A64:      [[#IDCALL]]: {{.*}} adr x9
# A64:      [[#IECALL]]: {{.*}} adr x9
# A64-NEXT: stp x30, x9, [sp, #-0x10]!
# A64-NEXT: bl 0x[[#EENSURE]]
# A64:      [[#SGUARD]]: {{.*}} adrp x12, 0x[[#GATES]]
## A site in its function's prologue branches to its guard, which keeps the
## prologue's length.
# A64-LABEL: <proto>:
# A64-NEXT: [[#%x,PSITE:]]: {{.*}} b 0x[[#%x,PGUARD:]]
# A64-NEXT: stp x29, x30, [sp, #-0x10]!
# A64:      [[#PGUARD]]: {{.*}} adrp x8, 0x[[#GATES]]
# A64-LABEL: <long>:
# A64-COUNT-124: nop
# A64-NEXT: b 0x[[#%x,LGUARD:]]
## In a section that may pass 1 MB, the guard computes the site's address
## by adrp and add, and an inline site reaches it by tbz over a b.
# A64-LABEL: <wide>:
# A64-NEXT: [[#%x,WSITE:]]: {{.*}} adrp x8, 0x[[#GATES]]
# A64-NEXT: ldr x8, [x8, #0x[[#DGATE]]]
# A64-NEXT: tbz w8, #0x0, 0x[[#%x,WLOAD:]]
# A64-NEXT: b 0x[[#%x,WCALL:]]
# A64-NEXT: [[#WLOAD]]: {{.*}} adrp x8
# A64:      [[#WCALL]]: {{.*}} adrp x8, 0x180001000
# A64-NEXT: add x8, x8, #0x[[#%x,WSITE-0x180001000]]
## An inline site whose tbnz reaches its block takes it, whatever its
## section's size.
# A64-LABEL: <deep>:
# A64:      adrp x8, 0x[[#GATES]]
# A64-NEXT: ldr x8, [x8, #0x[[#DGATE]]]
# A64-NEXT: tbnz w8, #0x0
## A foreign object's read of an import's entry is guarded; its load for a
## jump is not.
# A64-LABEL: <other>:
# A64-NEXT: b 0x[[#%x,OGUARD:]]
# A64-NEXT: ldr x0, [x8, #0x8]
# A64-NEXT: adrp x16
# A64-NEXT: ldr x16, [x16, #0x10]
# A64-NEXT: br x16
# A64-NEXT: [[#OGUARD]]: {{.*}} adrp x8, 0x[[#GATES]]

## Two blocks' records for the guard of two gates: head and call each, then
## the tail.
# A64:      Function: 0x[[#%X,GUARD]]
# A64:        FunctionLength: 20
# A64:      Function: 0x[[#%X,GUARD+20]]
# A64:        FunctionLength: 8
# A64:          0xec ; clear unwound to call
# A64:      Function: 0x[[#%X,GUARD+28]]
# A64:        FunctionLength: 20
# A64:      Function: 0x[[#%X,GUARD+48]]
# A64:        FunctionLength: 8
# A64:      Function: 0x[[#%X,GUARD+56]]
# A64:        FunctionLength: 8
## A record longer than 31 words takes the header's second word.
# A64:      Function: 0x[[#%X,LGUARD]]
# A64:        ByteCodeLength: 128
# A64-NEXT:   Prologue [
# A64-NEXT:     0xe5 ; end_c
# A64-COUNT-124: 0xe3 ; nop
# A64-NEXT:     0xe4 ; end
## The far site's address makes its head 6 instructions.
# A64:      Function: 0x[[#%X,WCALL-12]]
# A64:        FunctionLength: 24

## Where a block's bl does not reach its ensure stub, past 128 MB, it calls a
## veneer after the b back, an adrp, add and br of the site's register, which
## no record covers: ntdll unwinds it as a leaf, back to the call record.
# RUN: %{mc} -triple=aarch64-windows-itanium big-a64.s -o big-a64.obj
# RUN: %{link} d-a64.lib e-a64.lib reads-a64.obj rt-a64.obj big-a64.obj \
# RUN:   -include:big -out:far.dll
# RUN: llvm-objdump -d --start-address=0x180001068 --stop-address=0x1800010c0 \
# RUN:   far.dll > far.txt
# RUN: llvm-readobj --unwind far.dll >> far.txt
# RUN: FileCheck --check-prefix=FAR %s < far.txt
# FAR:      adr x8, 0x180001000
# FAR-NEXT: stp x30, x8, [sp, #-0x10]!
# FAR-NEXT: [[#%x,CALL:]]: {{.*}} bl 0x[[#%x,VENEER:]]
# FAR-NEXT: ldr x30, [sp], #0x10
# FAR:      b 0x180001004
# FAR-NEXT: [[#VENEER]]: {{.*}} adrp x8
# FAR-NEXT: add x8, x8
# FAR-NEXT: br x8
# FAR:      Function: 0x[[#%X,CALL]]
# FAR:        FunctionLength: 8
# FAR:          0xec ; clear unwound to call
# FAR-NOT:  Function: 0x[[#%X,VENEER]]

## A site that reaches a delay-bound section other than through a page is
## an error.
# RUN: %{mc} -triple=aarch64-windows-itanium reads-a64.s --defsym BAD=1 \
# RUN:   -o bad-a64.obj
# RUN: not %{link} d-a64.lib e-a64.lib bad-a64.obj rt-a64.obj -include:bad \
# RUN:   -out:bad.dll 2>&1 | FileCheck --check-prefix=BAD %s
# BAD: error: bad-a64.obj: .text+0x0: IMAGE_REL_ARM64_REL21 reaches t1, which holds the address of a function of a delay-loaded DLL, in an instruction that does not compute its address from a page, which the link cannot guard

# RUN: lld-link -machine:x64 -def:d.def -out:d.lib
# RUN: lld-link -machine:x64 -def:e.def -out:e.lib
# RUN: %{mc} -triple=x86_64-windows-itanium -mattr=+egpr x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium -I %S/Inputs rt.s -o rt.obj
# RUN: %{mc} -triple=x86_64-windows-msvc foreign.s -o foreign.obj
# RUN: %{mc} -triple=x86_64-windows-msvc giats.s -o giats.obj
# RUN: %{link} d.lib e.lib x64.obj rt.obj foreign.obj giats.obj -export:other \
# RUN:   -export:gia -out:x64.dll
# RUN: llvm-objdump -d x64.dll | FileCheck --check-prefix=X64 %s
# RUN: %{mc} -triple=x86_64-windows-msvc giats.s --defsym NOGIATS=1 \
# RUN:   -o nogiats.obj
# RUN: %{link} d.lib e.lib x64.obj rt.obj foreign.obj nogiats.obj \
# RUN:   -export:other -export:gia -out:nogiats.dll
# RUN: llvm-objdump -d nogiats.dll | FileCheck --check-prefix=NOGIATS %s

## The address 8 bytes into f calls a leaf in its 7 bytes, which loads
## f$ap8's entry, the name d.dll exports for it, behind d.dll's gate. A
## frstor, whose operand states no width, reaches u's word and is replayed;
## a load that misses the word stays.
# X64:      <.text>:
# X64-NEXT: callq 0x[[#%x,OFF:]]
# X64-NEXT: nop
# X64-NEXT: callq 0x[[#%x,WIDTH:]]
# X64-NEXT: nop
# X64-NEXT: movl {{.*}}(%rip), %eax
## A lea behind REX2, of f and of the address inside it, calls a leaf in its
## 8 bytes.
# X64-NEXT: callq 0x[[#%x,REX2:]]
# X64-NEXT: nopl (%rax)
# X64-NEXT: callq
# X64-NEXT: nopl (%rax)
## A foreign object without addrsig or .gfids observes k, whose entry it reads;
## one with .gfids observes k2 through .giats.
# X64:      <other>:
# X64-NEXT: callq
# X64-NEXT: nop
# X64-NEXT: callq *
# X64:      <gia>:
# X64-NEXT: callq
# X64-NEXT: nop
# X64:      [[#OFF]]: 50 pushq %rax
# X64:      movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,GATE:]]
# X64:      movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,FAP8:]]
# X64:      [[#WIDTH]]: 51 pushq %rcx
# X64:      frstor {{.*}}(%rip)
# X64-NEXT: retq
# X64:      [[#REX2]]: 51 pushq %rcx
# X64:      d5 48 8b 0d {{.*}} movq {{.*}}(%rip), %r17 {{.*}}# 0x[[#GATE]]
# X64:      d5 48 8b 0d {{.*}} movq {{.*}}(%rip), %r17 {{.*}}# 0x[[#FAP8-8]]

# NOGIATS:      <gia>:
# NOGIATS-NEXT: movq {{.*}}(%rip), %rax

#--- d.def
LIBRARY d.dll
EXPORTS
call
f
f$ap8
g
k
k2

#--- e.def
LIBRARY e.dll
EXPORTS
e

#--- reads-a64.s
.text
.globl main
main:
  adrp x8, t1+8
  ldr x0, [x8, :lo12:t1+8]
  adrp x9, t1
  add x9, x9, :lo12:t1
  adrp x10, t2
  str x1, [x10, :lo12:t2]
  adrp x11, s+8
  ldr w2, [x11, :lo12:s+8]
  adrp x12, s
  ldr x3, [x12, :lo12:s]
  adrp x13, __imp_t2
  ldr x13, [x13, :lo12:__imp_t2]
  bl call
  ret
.section .text,"xr",one_only,proto
.globl proto
.seh_proc proto
proto:
  adrp x8, __imp_f
  .seh_nop
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  .seh_endprologue
  ldr x0, [x8, :lo12:__imp_f]
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
.seh_endproc
.section .text,"xr",one_only,long
.globl long
.seh_proc long
long:
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  nop
  .seh_nop
  .seh_endprologue
  adrp x8, __imp_f
  ldr x0, [x8, :lo12:__imp_f]
  ret
.seh_endproc
.section .text,"xr",one_only,wide
.globl wide
wide:
  adrp x8, __imp_f
  ldr x8, [x8, :lo12:__imp_f]
  ret
  .space 0x80000
.section .text,"xr",one_only,deep
.globl deep
deep:
  .space 0x5000
  adrp x8, __imp_f
  ldr x8, [x8, :lo12:__imp_f]
  ret
.ifdef BAD
.section .text,"xr",one_only,bad
.globl bad
bad:
  adr x0, t1
  ret
.endif
.addrsig
.addrsig_sym f
.addrsig_sym g
.addrsig_sym e
.globl t1, t2, s
.section .rdata,"dr",one_only,t1
t1:
  .quad f
  .quad e
.data
.p2align 3
t2:
  .quad g
.section .rdata,"dr",one_only,s
s:
  .quad g
  .long 5
.section .drectve,"yn"
  .ascii " -include:t1 -include:s"

#--- foreign-a64.s
.section .text,"xr",one_only,other
.globl other
other:
  adrp x8, __imp_f
  ldr x0, [x8, :lo12:__imp_f]
  adrp x16, __imp_g
  ldr x16, [x16, :lo12:__imp_g]
  br x16

#--- x64.s
.text
.globl main
main:
  leaq f+8(%rip), %rcx
  frstor u-100(%rip)
  movl u+8(%rip), %eax
  leaq f(%rip), %r17
  leaq f+8(%rip), %r18
  callq call
  ret
  movq __imp_call@FUSED(%rip), %rax
.addrsig
.addrsig_sym f
.addrsig_sym g
.data
.p2align 3
.globl u
u:
  .quad g
  .quad 5

#--- foreign.s
  .def other; .scl 2; .type 32; .endef
  .text
  .globl other
other:
  movq __imp_k(%rip), %rax
  callq *__imp_g(%rip)
  retq

#--- giats.s
  .def @feat.00; .scl 3; .type 0; .endef
  .globl @feat.00
  .set @feat.00, 0x800
  .def gia; .scl 2; .type 32; .endef
  .text
  .globl gia
gia:
  movq __imp_k2(%rip), %rax
  retq
  .section .gfids$y,"dr"
  .symidx gia
.ifndef NOGIATS
  .section .giats$y,"dr"
  .symidx __imp_k2
.endif

#--- big-a64.s
.section .text$big,"xr"
.globl big
big:
  ret
  .space 0x8000000

#--- rt.s
.text
.globl __llvm_delay_resolve
__llvm_delay_resolve:
.globl __delayLoadHelper2
__delayLoadHelper2:
  ret
.data
.globl __MSSafeDelayLoadIAT
__MSSafeDelayLoadIAT:
  .long 0
.include "loadconfig-cfg-iat.s"
