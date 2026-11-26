# REQUIRES: x86, aarch64

## Jumps that end their section at another function's entry chain the two so
## the jump is deleted or short; chains follow /call-graph-profile-sort: when a
## profile counts calls, and /order: or a foreign object turns both off.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab \
# DEFINE:   -Brepro
# DEFINE: %{dis} = llvm-objdump -d --no-show-raw-insn
# RUN: %{mc} -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium --defsym=PROFILE=1 x64.s \
# RUN:   -o profile.obj
# RUN: %{mc} -triple=x86_64-windows-msvc --defsym=FOREIGN=1 x64.s -o foreign.obj
# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj

# RUN: %{link} x64.obj -out:x64.exe
# RUN: llvm-objdump -d x64.exe > x64.txt
# RUN: %{link} x64.obj -out:cdsort.exe -call-graph-profile-sort:cdsort
# RUN: cmp x64.exe cdsort.exe
# RUN: %{link} x64.obj -out:bare.exe -call-graph-profile-sort:no \
# RUN:   -call-graph-profile-sort
# RUN: cmp x64.exe bare.exe
# RUN: %{link} x64.obj -out:hfsort.exe -call-graph-profile-sort:hfsort
# RUN: cmp x64.exe hfsort.exe
# RUN: %{link} x64.obj -out:none.exe -call-graph-profile-sort:none
# RUN: cmp x64.exe none.exe
# RUN: %{link} x64.obj -out:no.exe -call-graph-profile-sort:no
# RUN: cmp x64.exe no.exe

## No sort runs: a chain lies where its first section does, and no other
## section moves.
# RUN: llvm-nm -n x64.exe | FileCheck --check-prefix=UNSORTED %s
# UNSORTED:      T a
# UNSORTED-NEXT: T b
# UNSORTED-NEXT: T c
# UNSORTED-NEXT: T d
# UNSORTED-NEXT: T x
# UNSORTED-NEXT: T y
# UNSORTED-NEXT: T mid
# UNSORTED-NEXT: T fixed
# UNSORTED-NEXT: T p1
# UNSORTED-NEXT: T t
# UNSORTED-NEXT: T p2
# UNSORTED-NEXT: T r
# UNSORTED-NEXT: T q
# UNSORTED-NEXT: t __cfi_k
# UNSORTED-NEXT: T k
# UNSORTED-NEXT: T via
# UNSORTED-NEXT: T w
# UNSORTED-NEXT: T direct
# UNSORTED-NEXT: T main

## a's and b's jumps are deleted and c's, after a pop, is two bytes.
# RUN: FileCheck --check-prefix=CHAIN --input-file=x64.txt %s
# CHAIN:      <a>:
# CHAIN-NEXT:   nop
# CHAIN-EMPTY:
# CHAIN-NEXT: <b>:
# CHAIN-NEXT:   nop
# CHAIN-EMPTY:
# CHAIN-NEXT: <c>:
# CHAIN-NEXT:   pushq %rbx
# CHAIN-NEXT:   popq %rbx
# CHAIN-NEXT:   eb 00 jmp {{.*}} <d>
# CHAIN-EMPTY:
# CHAIN-NEXT: <d>:

## y follows x, and its jump back stays.
# RUN: FileCheck --check-prefix=CYCLE --input-file=x64.txt %s
# CYCLE:      <x>:
# CYCLE-NEXT:   nop
# CYCLE-EMPTY:
# CYCLE-NEXT: <y>:
# CYCLE-NEXT:   nop
# CYCLE-NEXT:   jmp {{.*}} <x>

## mid's jump is not at its end, other's section has another name, fixed's is
## in a frame; p1's is first of two equal ones and falls into t, p2's stays.
# RUN: FileCheck --check-prefix=FIRST --input-file=x64.txt %s
# FIRST:      <p1>:
# FIRST-NEXT:   nop
# FIRST-EMPTY:
# FIRST-NEXT: <t>:
# FIRST:      <p2>:
# FIRST-NEXT:   nop
# FIRST-NEXT:   callq {{.*}} <t>
# FIRST-NEXT:   jmp {{.*}} <t>

## k keeps its prefix, so q's jump stays, in two bytes.
# RUN: FileCheck --check-prefix=PREFIX --input-file=x64.txt %s
# PREFIX:      <q>:
# PREFIX-NEXT:   nop
# PREFIX-NEXT:   eb 04 jmp {{.*}} <k>
# PREFIX-EMPTY:
# PREFIX-NEXT: <__cfi_k>:

## via's jump through w's pointer is made direct and is the first to w.
# RUN: FileCheck --check-prefix=BOUND --input-file=x64.txt %s
# BOUND:      <via>:
# BOUND-NEXT:   nop
# BOUND-EMPTY:
# BOUND-NEXT: <w>:
# BOUND:      <direct>:
# BOUND-NEXT:   nop
# BOUND-NEXT:   jmp {{.*}} <w>

## A deletable jump wins over an earlier short-only one (second falls into u);
## nothing falls into pre past its prefix, so pre falls into back.
# RUN: %{mc} -triple=x86_64-windows-itanium tie.s -o tie.obj
# RUN: %{link} tie.obj -out:tie.exe
# RUN: %{dis} tie.exe | FileCheck --check-prefix=TIE %s
# TIE:      <first>:
# TIE-NEXT:   pushq %rbx
# TIE-NEXT:   popq %rbx
# TIE-NEXT:   jmp {{.*}} <u>
# TIE-EMPTY:
# TIE-NEXT: <second>:
# TIE-NEXT:   nop
# TIE-EMPTY:
# TIE-NEXT: <u>:
# TIE:      <pre>:
# TIE-NEXT:   nop
# TIE-EMPTY:
# TIE-NEXT: <back>:
# TIE-NEXT:   nop
# TIE-NEXT:   jmp {{.*}} <pre>

## The profile counts 50 calls of t by p2, which calls it twice, and 30 by p1.
# RUN: %{link} profile.obj -out:profile.exe
# RUN: %{dis} profile.exe | FileCheck --check-prefix=PROFILE %s
# PROFILE:      <p1>:
# PROFILE-NEXT:   nop
# PROFILE-EMPTY:
# PROFILE-NEXT: <t>:

## Either sort places the counted pairs first and keeps the other chains; the
## pair of other and mid, in two $ groups, is dropped.
# RUN: llvm-nm -n profile.exe | FileCheck --check-prefix=SORTED %s
# RUN: %{link} profile.obj -out:profile-hfsort.exe \
# RUN:   -call-graph-profile-sort:hfsort
# RUN: llvm-nm -n profile-hfsort.exe | FileCheck --check-prefix=SORTED %s
# SORTED:      T p2
# SORTED-NEXT: T p1
# SORTED-NEXT: T t
# SORTED-NEXT: T a
# SORTED:      T y
# SORTED-NEXT: T mid

## Counted 70 times, p2's calls weigh 35 each.
# RUN: %{mc} -triple=x86_64-windows-itanium --defsym=PROFILE=1 \
# RUN:   --defsym=MORE=1 x64.s -o profile2.obj
# RUN: %{link} profile2.obj -out:profile2.exe
# RUN: %{dis} profile2.exe | FileCheck --check-prefix=PROFILE2 %s
# PROFILE2:      <p2>:
# PROFILE2-NEXT:   nop
# PROFILE2-NEXT:   callq {{.*}} <t>
# PROFILE2-EMPTY:
# PROFILE2-NEXT: <t>:

## With /order:, and for an object that states no calls, the sections keep
## the objects' order; /order: places what it names first.
# RUN: %{link} foreign.obj -out:foreign.exe
# RUN: llvm-nm -n foreign.exe | FileCheck --check-prefix=NONE %s
# RUN: echo t > order.txt
# RUN: %{link} x64.obj -out:order.exe -order:@order.txt
# RUN: llvm-nm -n order.exe | FileCheck --check-prefix=ORDER %s
# NONE:       T d
# NONE-NEXT:  T c
# NONE-NEXT:  T b
# NONE-NEXT:  T a
# NONE-NEXT:  T x
# NONE-NEXT:  T y
# NONE-NEXT:  T t
# ORDER:      T t
# ORDER-NEXT: T d
# ORDER-NEXT: T c
# ORDER-NEXT: T b
# ORDER-NEXT: T a

# RUN: not %{link} x64.obj -out:bad.exe -call-graph-profile-sort:c3 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD %s
# BAD: error: unknown /call-graph-profile-sort: value: c3

## early calls e1 with its first instruction and late calls e2 with its last.
## The sort knows where: e1 goes before its caller.
# RUN: %{mc} -triple=x86_64-windows-itanium offset.s -o offset.obj
# RUN: %{link} offset.obj -out:offset.exe
# RUN: llvm-nm -n offset.exe | FileCheck --check-prefix=OFFSET %s
# OFFSET:      T e2
# OFFSET-NEXT: T e1
# OFFSET-NEXT: T early
# OFFSET-NEXT: T late

## c1's jump to X, a function override, joins no chain: c1 stays behind X.
# RUN: %{mc} -triple=x86_64-windows-itanium -I %S/Inputs fo.s -o fo.obj
# RUN: %{mc} -triple=x86_64-windows-itanium %S/Inputs/loadconfig-cfg-x64.s \
# RUN:   -o loadconfig.obj
# RUN: lld-link -dll -noentry -export:X -funcoverride -guard:cf fo.obj \
# RUN:   loadconfig.obj -out:fo.dll -map:fo.map
# RUN: FileCheck --check-prefix=FO --input-file=fo.map %s
# FO: 0001:00000001 c1
# FO: 0001:00000000 body

## a's b is deleted, though r calls c too; c, whose function has unwind data,
## keeps its own.
# RUN: %{link} -machine:arm64 a64.obj -out:a64.exe
# RUN: %{dis} a64.exe | FileCheck --check-prefix=A64 %s
# A64:      <a>:
# A64-NEXT:   nop
# A64-EMPTY:
# A64-NEXT: <c>:
# A64-NEXT:   nop
# A64-NEXT:   b {{.*}} <d>
## Pointer jumps after an epilogue (first, third) are made direct and stay;
## second's b and fourth's pointer jump are deleted.
# RUN: %{mc} -triple=aarch64-windows-itanium tie-a64.s -o tie-a64.obj
# RUN: %{link} -machine:arm64 tie-a64.obj -out:tie-a64.exe
# RUN: %{dis} tie-a64.exe | FileCheck --check-prefix=TIE-A64 %s
# TIE-A64:      <first>:
# TIE-A64-NEXT:   sub sp, sp, #0x10
# TIE-A64-NEXT:   add sp, sp, #0x10
# TIE-A64-NEXT:   b {{.*}} <u>
# TIE-A64-EMPTY:
# TIE-A64-NEXT: <second>:
# TIE-A64-NEXT:   nop
# TIE-A64-EMPTY:
# TIE-A64-NEXT: <u>:
# TIE-A64:      <third>:
# TIE-A64-NEXT:   sub sp, sp, #0x10
# TIE-A64-NEXT:   add sp, sp, #0x10
# TIE-A64-NEXT:   b {{.*}} <v>
# TIE-A64-EMPTY:
# TIE-A64-NEXT: <fourth>:
# TIE-A64-NEXT:   nop
# TIE-A64-EMPTY:
# TIE-A64-NEXT: <v>:

## An object LLVM did not make for these targets states no calls beside one
## that does, either: its sections keep their order.
# RUN: %{mc} -triple=aarch64-windows-msvc foreign-a64.s -o foreign-a64.obj
# RUN: %{link} -machine:arm64 a64.obj foreign-a64.obj -out:mixed.exe
# RUN: llvm-nm -n mixed.exe | FileCheck --check-prefix=MIXED %s
# MIXED:      T callee
# MIXED-NEXT: T caller

## In a link that holds no such object, a section that no symbol leads keeps
## its place, though the profile counts its calls.
# RUN: %{mc} -triple=x86_64-windows-msvc n1.s -o n1.obj
# RUN: %{mc} -triple=x86_64-windows-msvc n2.s -o n2.obj
# RUN: lld-link -entry:N1 -subsystem:console -debug:symtab n1.obj n2.obj \
# RUN:   -out:msvc.exe
# RUN: llvm-nm -n msvc.exe | FileCheck --check-prefix=MSVC %s
# MSVC:      T N1
# MSVC-NEXT: T N2

#--- fn.inc
  .macro fn name
  .def \name; .scl 2; .type 32; .endef
  .section .text,"xr",discard,\name
  .globl \name
\name:
  .endm

#--- x64.s
  .include "fn.inc"
## Code moves only after a rewrite deletes an instruction such as a fused read;
## only there does a jump stay two bytes.
  .macro read p
.ifndef FOREIGN
  movq __imp_\p@FUSED(%rip), %rax
.endif
  .endm

  fn d
  retq

  fn c
  .seh_proc c
  pushq %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  read d
  .seh_startepilogue
  popq %rbx
  .seh_endepilogue
  jmp d
  .seh_endproc

  fn b
  nop
  jmp c

  fn a
  nop
  jmp b

  fn x
  nop
  jmp y
  fn y
  nop
  jmp x

  fn t
  retq
## Jumps to t that join no chain: inside a section, from a section of another
## name, and from a section that is not resizable.
  fn mid
  jmp t
  nop
  .def other; .scl 2; .type 32; .endef
  .section .text$o,"xr",discard,other
  .globl other
other:
  nop
  jmp t
  fn fixed
  .seh_proc fixed
  .seh_endprologue
  jmp t
  .seh_endproc
  fn p1
  nop
  jmp t
  fn p2
  nop
  read t
  callq t
  jmp t

  .def k; .scl 2; .type 32; .endef
  .section .text,"xr",discard,k
  .globl k
__cfi_k:
  .long 0
k:
  retq
  fn r
  callq k
  retq
  fn q
  nop
  read k
  jmp k

## A jump through w's pointer, which the scan makes direct, and after it a
## direct one.
  fn w
  retq
  fn via
  nop
  jmpq *__imp_w(%rip)
  fn direct
  nop
  jmp w

  fn main
  callq via
  callq direct
  callq a
  callq x
  callq p1
  callq p2
  callq q
  callq r
  callq mid
  callq other
  callq fixed
  retq

.ifdef PROFILE
  .cg_profile other, mid, 1000
  .cg_profile p1, t, 30
.ifdef MORE
  .cg_profile p2, t, 70
.else
  .cg_profile p2, t, 50
.endif
.endif

#--- tie.s
  .include "fn.inc"
  fn u
  retq
  fn first
  .seh_proc first
  pushq %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  movq __imp_u@FUSED(%rip), %rax
  .seh_startepilogue
  popq %rbx
  .seh_endepilogue
  jmp u
  .seh_endproc
  fn second
  nop
  jmpq *__imp_u(%rip)

  fn back
  nop
  jmp pre
  .def pre; .scl 2; .type 32; .endef
  .section .text,"xr",discard,pre
  .globl pre
__cfi_pre:
  .long 0
pre:
  nop
  jmp back

  fn main
  callq first
  callq second
  callq back
  retq

#--- offset.s
  .include "fn.inc"
  fn e1
  retq
  fn e2
  retq
  fn early
  callq e1
  .fill 3000, 1, 0x90
  retq
  fn late
  .fill 3000, 1, 0x90
  callq e2
  retq
  fn main
  retq
  .cg_profile early, e1, 100
  .cg_profile late, e2, 100

#--- fo.s
  .def X; .scl 2; .type 32; .endef
  .section .text,"xr",discard,X
  .globl X
X:
body:
  retq
  .section .text,"xr",discard,c1
  .globl c1
c1:
  nop
  jmp X
  .include "function-override-x.s"
  .rva c1, X

#--- a64.s
  .include "fn.inc"
  fn d
  ret

  fn c
  .seh_proc c
  .seh_endprologue
  nop
  b d
  .seh_endproc

  fn r
  bl c
  ret

  fn a
  nop
  b c

  fn main
  bl r
  bl a
  ret

#--- tie-a64.s
  .include "fn.inc"
## A fused jump after an epilogue, which stays where it is made direct.
  .macro stays name, to
  fn \name
  .seh_proc \name
  sub sp, sp, #16
  .seh_stackalloc 16
  .seh_endprologue
  .seh_startepilogue
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  adrp x16, __imp_\to
  ldr x16, [x16, :fused_lo12:__imp_\to]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_\to
  br x16
  .seh_endproc
  .endm

  fn u
  ret
  stays first, u
  fn second
  nop
  b u

  fn v
  ret
  stays third, v
  fn fourth
  nop
  adrp x16, __imp_v
  ldr x16, [x16, :fused_lo12:__imp_v]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_v
  br x16

  fn main
  bl first
  bl second
  bl third
  bl fourth
  ret

#--- foreign-a64.s
  .section .text,"xr",discard,callee
  .globl callee
callee:
  ret
  .section .text,"xr",discard,caller
  .globl caller
caller:
  nop
  bl callee
  ret

#--- n1.s
  .text
  .globl N1
N1:
  retq

#--- n2.s
  .text
  .globl N2
N2:
  retq
  .cg_profile N2, N1, 100
