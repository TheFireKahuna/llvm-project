# REQUIRES: x86

## A compiler describes a call through the register that a load of an import
## pointer fills, padded to the length of the same call through the pointer,
## and the load. A call whose pointer leads out of the image, or holds an
## absent weak function's zero, stays as it is: a call through the register
## after segment overrides, or REX.W jmp through it before int3. A call to a
## function in the image becomes direct, and one through a delay-loaded
## import's pointer becomes a call through the pointer. A load that only fused
## calls read is then dead: it becomes segment overrides on the instruction
## that follows it, or one no-operation where that instruction is too long,
## transfers control across a 32-byte boundary, or is unknown. Nothing moves.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium late.s -o late.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium helper.s \
# RUN:   -o helper.obj
# RUN: llc -mtriple=x86_64-unknown-windows-itanium \
# RUN:   -start-before=x86-windows-import-fusion -filetype=obj jump.mir \
# RUN:   -o jump.obj
# RUN: lld-link -dll -noentry dll.obj -out:d.dll
# RUN: lld-link -dll -noentry late.obj -out:late.dll

# RUN: lld-link -guard:cf -entry:main -subsystem:console -out:a.exe \
# RUN:   -map:a.map main.obj helper.obj d.lib late.lib -delayload:late.dll
# RUN: llvm-objdump -d a.exe | FileCheck %s
# RUN: llvm-readobj --coff-load-config a.exe > lc.txt
# RUN: cat a.map lc.txt | FileCheck %s --check-prefix=GUARD

## The same link with -import-slots.
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:b.exe \
# RUN:   main.obj helper.obj d.lib late.lib -delayload:late.dll
# RUN: llvm-objdump -d b.exe | FileCheck %s

## A compiler's jumps through one of the next eight registers.
# RUN: lld-link -entry:jumpext -subsystem:console -out:e.exe jump.obj \
# RUN:   main.obj helper.obj d.lib late.lib -delayload:late.dll
# RUN: llvm-objdump -d e.exe | FileCheck %s --check-prefix=JUMP


## ext is in d.dll: its call goes through rbx, and the load stays.
# CHECK-LABEL: <.text>:
# CHECK-NEXT:  48 8b 1d {{.*}} movq {{.*}}(%rip), %rbx
# CHECK-NEXT:  31 c0 xorl %eax, %eax
# CHECK-NEXT:  2e 2e 2e 2e ff d3 callq *%rbx
## local is in the image: its call is direct, and its dead load is segment
## overrides on xorl.
# CHECK-NEXT:  2e 2e 2e 2e 2e 2e 2e 31 c0 xorl %eax, %eax
# CHECK-NEXT:  67 e8 {{.*}} addr32 callq 0x[[#%x,LOCAL:]]
## Through r9, after REX.B.
# CHECK-NEXT:  4c 8b 0d {{.*}} movq {{.*}}(%rip), %r9
# CHECK-NEXT:  2e 2e 2e 41 ff d1 callq *%r9
## A dead load followed by another fused load has nothing to carry its bytes,
## and is one no-operation.
# CHECK-NEXT:  0f 1f 80 00 00 00 00 nopl (%rax)
# CHECK-NEXT:  48 8b 35 {{.*}} movq {{.*}}(%rip), %rsi
# CHECK-NEXT:  67 e8 {{.*}} addr32 callq 0x[[#LOCAL]]
# CHECK-NEXT:  2e 2e 2e 2e ff d6 callq *%rsi
## late.dll is delay-loaded: the call reads the pointer, and the load is dead.
# CHECK-NEXT:  2e 2e 2e 2e 2e 2e 2e 31 c9 xorl %ecx, %ecx
# CHECK-NEXT:  ff 15 {{.*}} callq *{{.*}}(%rip)
## The guarded call of an absent function goes through the zero its test
## loaded.
# CHECK-NEXT:  48 c7 c0 00 00 00 00 movq $0x0, %rax
# CHECK-NEXT:  48 85 c0 testq %rax, %rax
# CHECK-NEXT:  74 06 je
# CHECK-NEXT:  2e 2e 2e 2e ff d0 callq *%rax
# CHECK-NEXT:  c3 retq

## A tail jump to ext through rax stays; one to local becomes direct at its
## first byte, and one to lateext reads the pointer.
# CHECK:       48 8b 05 {{.*}} movq {{.*}}(%rip), %rax
# CHECK-NEXT:  48 85 c0 testq %rax, %rax
# CHECK-NEXT:  48 ff e0 jmpq *%rax
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  2e 2e 2e 2e 2e 2e 2e 45 31 c0 xorl %r8d, %r8d
# CHECK-NEXT:  e9 {{.*}} jmp 0x[[#LOCAL]]
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  2e 2e 2e 2e 2e 2e 2e 45 31 c0 xorl %r8d, %r8d
# CHECK-NEXT:  48 ff 25 {{.*}} jmpq *{{.*}}(%rip)

## The guarded tail jump to an absent function goes through the zero.
# CHECK:       48 c7 c0 00 00 00 00 movq $0x0, %rax
# CHECK-NEXT:  48 85 c0 testq %rax, %rax
# CHECK-NEXT:  74 07 je
# CHECK-NEXT:  48 ff e0 jmpq *%rax
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  cc int3
# CHECK-NEXT:  c3 retq

## A dead load before an instruction that may fuse with a branch, which it
## would carry across a 32-byte boundary, is one no-operation; one that would
## not is overrides.
# CHECK-COUNT-29: 90 nop
# CHECK-NEXT:  0f 1f 80 00 00 00 00 nopl (%rax)
# CHECK-NEXT:  48 85 c0 testq %rax, %rax
# CHECK-NEXT:  67 e8 {{.*}} addr32 callq 0x[[#LOCAL]]
# CHECK-NEXT:  2e 2e 2e 2e 2e 2e 2e 48 85 c0 testq %rax, %rax
# CHECK-NEXT:  67 e8 {{.*}} addr32 callq 0x[[#LOCAL]]

## Only main's address is taken: the loads of local's pointer are fused.
# GUARD:      0001:00000000 main [[#%x,MAIN:]]
# GUARD:      GuardFidTable [
# GUARD-NEXT:   0x[[#MAIN]]
# GUARD-NEXT: ]

## A compiler's tail jump to ext through r11 stays, after REX.W with B; one to
## local becomes direct, and the load its test reads computes local's address.
## The compiler's code is covered, so the int3 that padded each jump goes, and
## the branch over it is shorter.
# JUMP-LABEL: <.text>:
# JUMP-NEXT:  4c 8b 1d {{.*}} movq {{.*}}(%rip), %r11
# JUMP-NEXT:  4d 85 db testq %r11, %r11
# JUMP-NEXT:  74 03 je
# JUMP-NEXT:  49 ff e3 jmpq *%r11
# JUMP-NEXT:  c3 retq
# JUMP:       4c 8d 1d {{.*}} leaq {{.*}}(%rip), %r11 # 0x[[#%x,JLOCAL:]]
# JUMP-NEXT:  4d 85 db testq %r11, %r11
# JUMP-NEXT:  74 05 je
# JUMP-NEXT:  e9 {{.*}} jmp 0x[[#JLOCAL]]
# JUMP-NEXT:  c3 retq

#--- dll.s
  .text
  .globl ext
ext:
  retq
  .section .drectve,"yni"
  .ascii " -export:ext"

#--- main.s
  .def @feat.00; .scl 3; .type 0; .endef
  .globl @feat.00
  .set @feat.00, 0x800
  .text
  .def main; .scl 2; .type 32; .endef
  .globl main
main:
.Lext:
  .linkfusedread
  movq __imp_ext(%rip), %rbx
  xorl %eax, %eax
  .linkfusedcall .Lext, %rbx
  cs; cs; cs; cs
  callq *%rbx
.Llocal:
  .linkfusedread
  movq __imp_local(%rip), %rcx
  xorl %eax, %eax
  .linkfusedcall .Llocal, %rcx
  cs; cs; cs; cs
  callq *%rcx
.Lr9:
  movq __imp_ext(%rip), %r9
  .linkfusedcall .Lr9, %r9
  cs; cs; cs
  callq *%r9
.Lfirst:
  .linkfusedread
  movq __imp_local(%rip), %rdx
.Lsecond:
  .linkfusedread
  movq __imp_ext(%rip), %rsi
  .linkfusedcall .Lfirst, %rdx
  cs; cs; cs; cs
  callq *%rdx
  .linkfusedcall .Lsecond, %rsi
  cs; cs; cs; cs
  callq *%rsi
.Llate:
  .linkfusedread
  movq __imp_lateext(%rip), %rdi
  xorl %ecx, %ecx
  .linkfusedcall .Llate, %rdi
  cs; cs; cs; cs
  callq *%rdi
.Labsent:
  movq __imp_absent(%rip), %rax
  testq %rax, %rax
  je 1f
  .linkfusedcall .Labsent, %rax
  cs; cs; cs; cs
  callq *%rax
1:
  retq

  .def tails; .scl 2; .type 32; .endef
  .globl tails
  .p2align 4
tails:
.Ltail:
  movq __imp_ext(%rip), %rax
  testq %rax, %rax
  .linkfusedcall .Ltail, %rax
  rex64 jmpq *%rax
  int3; int3; int3; int3
.Ltail2:
  .linkfusedread
  movq __imp_local(%rip), %rcx
  xorl %r8d, %r8d
  .linkfusedcall .Ltail2, %rcx
  rex64 jmpq *%rcx
  int3; int3; int3; int3
.Ltail3:
  .linkfusedread
  movq __imp_lateext(%rip), %rdx
  xorl %r8d, %r8d
  .linkfusedcall .Ltail3, %rdx
  rex64 jmpq *%rdx
  int3; int3; int3; int3

  .def tailabsent; .scl 2; .type 32; .endef
  .globl tailabsent
  .p2align 4
tailabsent:
.Ltailabsent:
  movq __imp_absent(%rip), %rax
  testq %rax, %rax
  je 1f
  .linkfusedcall .Ltailabsent, %rax
  rex64 jmpq *%rax
  int3; int3; int3; int3
1:
  retq

  .def boundary; .scl 2; .type 32; .endef
  .globl boundary
  .p2align 5
boundary:
  .fill 29, 1, 0x90
.Lcross:
  .linkfusedread
  movq __imp_local(%rip), %rax  # 0x1d to 0x24, then testq across 0x20
  testq %rax, %rax
  .linkfusedcall .Lcross, %rax
  cs; cs; cs; cs
  callq *%rax
.Lwithin:
  .linkfusedread
  movq __imp_local(%rip), %rax  # 0x2d to 0x34
  testq %rax, %rax
  .linkfusedcall .Lwithin, %rax
  cs; cs; cs; cs
  callq *%rax
  retq

  .def local; .scl 2; .type 32; .endef
  .globl local
  .p2align 4
local:
  retq
  # Alignment with data after the fused calls leaves .text uncovered, so the
  # linker keeps the length of each call, which this test checks; relax.s
  # checks the forms of covered code.
  .p2align 0, 0

  .weak absent

#--- late.s
  .text
  .globl lateext
lateext:
  retq
  .section .drectve,"yni"
  .ascii " -export:lateext"

#--- helper.s
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq

  .section .rdata,"dr"
  .globl _load_config_used
  .p2align 3
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 12, 1, 0
  .quad __guard_iat_table
  .quad __guard_iat_count
  .fill 80, 1, 0

#--- jump.mir
--- |
  declare dllimport void @ext()
  declare dllimport void @local()

  define void @jumpext() { ret void }
  define void @jumplocal() { ret void }
...
---
name: jumpext
tracksRegLiveness: true
body: |
  bb.0:
    successors: %bb.1, %bb.2
    $r11 = MOV64rm $rip, 1, $noreg, target-flags(x86-dllimport) @ext, $noreg :: (load (s64) from got)
    TEST64rr killed $r11, $r11, implicit-def $eflags
    JCC_1 %bb.2, 4, implicit $eflags
  bb.1:
    TAILJMPm64_REX $rip, 1, $noreg, target-flags(x86-dllimport) @ext, $noreg, csr_win64, implicit $rsp, implicit $ssp :: (load (s64) from got)
  bb.2:
    RET64
...
---
name: jumplocal
tracksRegLiveness: true
body: |
  bb.0:
    successors: %bb.1, %bb.2
    $r11 = MOV64rm $rip, 1, $noreg, target-flags(x86-dllimport) @local, $noreg :: (load (s64) from got)
    TEST64rr killed $r11, $r11, implicit-def $eflags
    JCC_1 %bb.2, 4, implicit $eflags
  bb.1:
    TAILJMPm64_REX $rip, 1, $noreg, target-flags(x86-dllimport) @local, $noreg, csr_win64, implicit $rsp, implicit $ssp :: (load (s64) from got)
  bb.2:
    RET64
...
