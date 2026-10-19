# REQUIRES: x86
## Under -import-slots, a load of the import pointer of data that a DLL's
## instance preempts, which the object describes as one whose register only
## reads of memory read, takes the address of the image's own copy, kept with
## no symbol, while every other reference reaches the instance. A copy serves
## only if it is read-only and its words hold only addresses that are the same
## in every image: here a function the DLL provides, whose word becomes an
## in-place slot, but not a string of its own object. An import that only such
## loads used needs no entry. A DLL's own value reads of immutable data it keeps
## preemptible read its instance directly, its address-takes reading the cell.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium only.s -o only.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium own.s -o own.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -out:main.exe -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d --print-imm-hex main.exe > main.txt
# RUN: llvm-objdump -s -j .rdata main.exe >> main.txt
# RUN: FileCheck --input-file=main.txt %s
# RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=IMPORTS %s

# LOG-DAG: Keeping the copy of tbl in main.obj for 2 value reads
# LOG-DAG: Preempting tbl in main.obj with its instance in lib.dll
# LOG-DAG: Keeping the copy of ftbl in main.obj for 1 value read
# LOG-NOT: Keeping the copy of {{stbl|mut}}

## The indexed read leas the copy, and the address-take loads the entry. The
## access that a fused read describes reads the copy directly.
# CHECK:      <.text>:
# CHECK-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#%x,TBL:]]
# CHECK-NEXT: 8b 04 88 movl (%rax,%rcx,4), %eax
# CHECK-NEXT: 48 8b 15 {{.*}} movq {{.*}}(%rip), %rdx
# CHECK-NEXT: 4c 8d 05 {{.*}} leaq {{.*}}(%rip), %r8
# CHECK-NEXT: 4d 8b 04 c8 movq (%r8,%rcx,8), %r8
# CHECK-NEXT: 4c 8b 0d {{.*}} movq {{.*}}(%rip), %r9
# CHECK-NEXT: 4d 8b 09 movq (%r9), %r9
# CHECK-NEXT: 4c 8b 15 {{.*}} movq {{.*}}(%rip), %r10
# CHECK-NEXT: 45 8b 12 movl (%r10), %r10d
# CHECK-NEXT: c3 retq
# CHECK-NEXT: 8b 05 {{.*}} movl {{.*}}(%rip), %eax # 0x[[#TBL+8]]
# CHECK-NEXT: c3 retq
# CHECK:      Contents of section .rdata:
# CHECK:      [[#%x,TBL]] 01000000 02000000 03000000 04000000

# IMPORTS:      Name: lib.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA:
# IMPORTS-NEXT: Symbol: mut
# IMPORTS-NEXT: Symbol: stbl
# IMPORTS-NEXT: Symbol: tbl
# IMPORTS-NEXT: }
# IMPORTS:      Name: lib.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA:
# IMPORTS-NEXT: Symbol: f
# IMPORTS-NEXT: }

## Where only value reads remain, the image imports nothing for the data. A
## read through another name of the group, liveness having discarded the copy,
## reads the same copy.
# RUN: lld-link -import-slots -entry:main -subsystem:console only.obj lib.lib \
# RUN:   -out:only.exe
# RUN: llvm-objdump -d only.exe | FileCheck --check-prefix=ONLY %s
# RUN: llvm-readobj --coff-imports only.exe | \
# RUN:   FileCheck --check-prefix=NOIMPORTS %s
# ONLY:      <.text>:
# ONLY-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#%x,COPY:]]
# ONLY-NEXT: 8b 04 88 movl (%rax,%rcx,4), %eax
# ONLY-NEXT: 48 8d 15 {{.*}} leaq {{.*}}(%rip), %rdx # 0x[[#COPY]]
# NOIMPORTS-NOT: lib.dll

## A DLL that keeps tbl preemptible reads its instance for the value read.
# RUN: lld-link -import-slots -dll -noentry own.obj crt.obj ntdll.lib \
# RUN:   -out:own.dll -verbose 2>&1 | FileCheck --check-prefix=OWNLOG %s
# RUN: llvm-objdump -d own.dll | FileCheck --check-prefix=OWN %s
# OWNLOG: Keeping the copy of tbl in own.obj for 1 value read
# OWN:      <.text>:
# OWN-NEXT: 48 8d 05 {{.*}} leaq {{.*}} <tbl>
# OWN-NEXT: 8b 04 88 movl (%rax,%rcx,4), %eax
# OWN-NEXT: 48 8b 15 {{.*}} movq

## A copy whose word holds an address inside another DLL's object serves where
## that DLL exports the address exactly, the word taking that name in place,
## and not otherwise.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium interior.s \
# RUN:   -o interior.obj
# RUN: lld-link -import-slots -entry:h -subsystem:console interior.obj lib.lib \
# RUN:   -out:interior.exe -verbose 2>&1 | FileCheck --check-prefix=ILOG %s
# RUN: llvm-objdump -d interior.exe | FileCheck --check-prefix=INTERIOR %s
# RUN: llvm-readobj --coff-imports interior.exe | \
# RUN:   FileCheck --check-prefix=IIMPORTS %s
# ILOG:     Keeping the copy of itbl in interior.obj for 1 value read
# ILOG-NOT: Keeping the copy of xtbl
# INTERIOR:      <.text>:
# INTERIOR-NEXT: 48 8d 05 {{.*}} leaq
# INTERIOR-NEXT: 48 8b 00 movq (%rax), %rax
# INTERIOR-NEXT: 48 8b 15 {{.*}} movq
# IIMPORTS:      Name: lib.dll
# IIMPORTS-NEXT: ImportLookupTableRVA:
# IIMPORTS-NEXT: ImportAddressTableRVA:
# IIMPORTS-NEXT: Symbol: xtbl
# IIMPORTS-NEXT: }
# IIMPORTS:      Name: lib.dll
# IIMPORTS-NEXT: ImportLookupTableRVA:
# IIMPORTS-NEXT: ImportAddressTableRVA:
# IIMPORTS-NEXT: Symbol: obj$so8
# IIMPORTS-NEXT: }

## Without -import-slots, nothing is preempted, and the loads are kept.
# RUN: lld-link -entry:main -subsystem:console main.obj lib.lib -out:plain.exe \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=PLAIN %s
# PLAIN-NOT: Keeping the copy

#--- lib.def
LIBRARY lib.dll
EXPORTS
  tbl DATA
  tbla DATA
  ftbl DATA
  stbl DATA
  mut DATA
  itbl DATA
  xtbl DATA
  obj DATA
  obj$so8 DATA
  f

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory

#--- main.s
  .section .rdata,"dr",discard,tbl
  .globl tbl
  .p2align 4
tbl:
  .long 1, 2, 3, 4

  .section .rdata,"dr",discard,ftbl
  .globl ftbl
  .p2align 3
ftbl:
  .quad f

  .section .rdata,"dr",discard,stbl
  .globl stbl
  .p2align 3
stbl:
  .quad .Lstr

  .section .rdata,"dr"
.Lstr:
  .asciz "s"

  .section .data,"dw",discard,mut
  .globl mut
  .p2align 2
mut:
  .long 5

  .text
  .globl main
main:
  .linkvalueonly
  movq __imp_tbl(%rip), %rax
  movl (%rax,%rcx,4), %eax
  movq __imp_tbl(%rip), %rdx
  .linkvalueonly
  movq __imp_ftbl(%rip), %r8
  movq (%r8,%rcx,8), %r8
  .linkvalueonly
  movq __imp_stbl(%rip), %r9
  movq (%r9), %r9
  .linkvalueonly
  movq __imp_mut(%rip), %r10
  movl (%r10), %r10d
  retq

  .globl g
g:
.Lr0:
  .linkvalueonly
  .linkfusedread
  movq __imp_tbl(%rip), %rax
  .linkfusedaccess .Lr0, %rax
  movl 8(%rax), %eax
  retq

#--- interior.s
  .section .rdata,"dr",discard,itbl
  .globl itbl
  .p2align 3
itbl:
  .quad obj+8

  .section .rdata,"dr",discard,xtbl
  .globl xtbl
  .p2align 3
xtbl:
  .quad obj+4

  .text
  .globl h
h:
  .linkvalueonly
  movq __imp_itbl(%rip), %rax
  movq (%rax), %rax
  .linkvalueonly
  movq __imp_xtbl(%rip), %rdx
  movq (%rdx), %rdx
  retq

#--- only.s
  .section .rdata,"dr",discard,tbl
  .globl tbl
  .p2align 2
tbl:
  .long 1, 2, 3, 4
  .globl tbla
  .set tbla, tbl

  .text
  .globl main
main:
  .linkvalueonly
  movq __imp_tbl(%rip), %rax
  movl (%rax,%rcx,4), %eax
  .linkvalueonly
  movq __imp_tbla(%rip), %rdx
  addl (%rdx,%rcx,4), %eax
  retq

#--- own.s
  .section .rdata,"dr",discard,tbl
  .globl tbl
  .p2align 2
tbl:
  .long 1, 2, 3, 4

  .text
  .globl fo
fo:
  .linkvalueonly
  movq __imp_tbl(%rip), %rax
  movl (%rax,%rcx,4), %eax
  movq __imp_tbl(%rip), %rdx
  retq

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the NtProtectVirtualMemory it seals read-only cells with.
  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  leaq __llvm_fill_list(%rip), %rax
  callq *__imp_NtProtectVirtualMemory(%rip)
  retq
