# REQUIRES: x86
## Under -import-slots, the address of a function with default visibility in a
## COMDAT that a DLL the image imports from offers is the DLL's instance, as an
## ELF shared object's definition preempts an executable's copy of an inline
## function: a described lea of it becomes a load of its import address table
## entry and a word of static data takes it in place, while calls keep the
## image's copy, which is discarded once no call reaches it and leaves the
## image's Control Flow Guard table. An object that may take the address in an
## instruction it does not describe keeps the copy as the address, with a
## warning, and so does a delay-loaded or ignored offer. An export of the
## function forwards to the DLL.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc undesc.s \
# RUN:   -o undesc.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc config.s \
# RUN:   -o config.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium helper.s \
# RUN:   -o helper.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64

# RUN: lld-link -import-slots -guard:cf -entry:main -subsystem:console \
# RUN:   main.obj config.obj lib.lib -out:out.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d out.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports --coff-load-config out.exe | \
# RUN:   FileCheck --check-prefix=IMPORTS %s

# LOG-DAG: Binding the address of f in main.obj to its instance in lib.dll
# LOG-DAG: Binding the address of g in main.obj to its instance in lib.dll

## f's copy stays for its call; g's, which nothing calls, is discarded.
# CHECK:      <.text>:
# CHECK-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x[[#%x,F:]]
# CHECK-NEXT: e8 {{.*}} callq 0x[[#%x,COPY:]]
# CHECK-NEXT: 48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx # 0x[[#F+8]]
# CHECK-NEXT: c3 retq
# CHECK:      [[#COPY]]: b8 11 11 11 11 movl $0x11111111, %eax
# CHECK-NOT:  22222222

# IMPORTS:      Name: lib.dll
# IMPORTS:      Symbol: f (0)
# IMPORTS-NEXT: Symbol: g (1)
# IMPORTS:      GuardCFFunctionCount: 0

## Without -import-slots, the image takes no offer: its f and g are their
## addresses and call targets.
# RUN: lld-link -guard:cf -entry:main -subsystem:console main.obj config.obj \
# RUN:   lib.lib -out:plain.exe
# RUN: llvm-readobj --coff-load-config plain.exe | \
# RUN:   FileCheck --check-prefix=PLAIN %s
# PLAIN: GuardCFFunctionCount: 2

## An object that does not describe its instructions keeps f's copy as its
## address.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   undesc.obj lib.lib -out:undesc.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d undesc.exe | FileCheck --check-prefix=KEPT %s
# WARN: warning: undesc.obj: may take the address of f, which lib.dll provides, in an instruction it does not describe, so the image keeps its own copy as its address; rebuild the object with clang
# KEPT:      <.text>:
# KEPT-NEXT: 48 8d 05 {{.*}} leaq
# KEPT-NEXT: e8
# KEPT-NEXT: 48 8b 0d {{.*}} movq

## A delay-load thunk is not the function's address, and an ignored offer is
## not taken: the image keeps its copies.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   helper.obj lib.lib -delayload:lib.dll -out:delay.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=NOBIND %s
# RUN: llvm-objdump -d delay.exe | FileCheck --check-prefix=LOCAL %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   lib.lib -ignore-offers:lib.dll -out:ignore.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=NOBIND %s
# RUN: llvm-objdump -d ignore.exe | FileCheck --check-prefix=LOCAL %s
# NOBIND-NOT: Binding the address
# LOCAL:      <.text>:
# LOCAL-NEXT: 48 8d 05 {{.*}} leaq
# LOCAL-NEXT: e8
# LOCAL-NEXT: 48 8d 0d {{.*}} leaq

## An export of the function forwards to the DLL's instance.
# RUN: lld-link -import-slots -dll -noentry main.obj lib.lib -export:f \
# RUN:   -out:fwd.dll
# RUN: llvm-readobj --coff-exports fwd.dll | FileCheck --check-prefix=FWD %s
# FWD:      Name: f
# FWD-NEXT: ForwardedTo: lib.f

#--- lib.def
LIBRARY lib.dll
EXPORTS
  f
  g

#--- main.s
  .def @feat.00; .scl 3; .type 0; .endef
  .globl @feat.00
@feat.00 = 0x800
  .linkbindings
  .section .text,"xr",discard,f
  .globl f
  .p2align 4
f:
  .linkbinding f, default
  movl $0x11111111, %eax
  retq

  .section .text,"xr",discard,g
  .globl g
  .p2align 4
g:
  .linkbinding g, default
  movl $0x22222222, %eax
  retq

  .text
  .globl main
  .p2align 4
main:
  leaq f(%rip), %rax
  callq f
  leaq g(%rip), %rcx
  retq

  .data
  .quad f
  .quad g

  .section .gfids$y,"dr"
  .symidx f
  .symidx g
  .addrsig
  .addrsig_sym f
  .addrsig_sym g

#--- undesc.s
  .text
  .globl h
h:
  leaq f(%rip), %rax
  retq

#--- helper.s
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq

#--- config.s
  .section .rdata,"dr"
  .p2align 3
  .globl _load_config_used
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 12, 1, 0
  .quad __guard_iat_table
  .quad __guard_iat_count
  .quad __guard_longjmp_table
  .quad __guard_fids_count
  .fill 84, 1, 0
