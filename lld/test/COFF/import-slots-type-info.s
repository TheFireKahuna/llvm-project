# REQUIRES: x86
## Under -import-slots, a DLL offers the type_info objects with vague linkage
## and default visibility that it keeps, as it offers other vague data, but
## not their names, which are reached through the objects. An importer's own
## copy is preempted by the offer: its code reaches the DLL's object through
## the import, and a word of its static data that holds the object's address,
## a kept vtable's or a base pointer in a derived type_info object that only
## the importer keeps, becomes an in-place slot. Unlike other vague data, a DLL
## reaches its own type_info object directly, through no cell: its fill writes
## only the object's name word, with the owner's where both name the same type
## and layout, so that true siblings that both keep one compare one word. The
## record of what it keeps lists the object for an executable to pick its
## owner, and true siblings that both keep it are not reported.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium twice.s \
# RUN:   -o twice.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib -out:a.dll \
# RUN:   -map:a.map
# RUN: llvm-readobj --coff-exports a.dll | FileCheck --check-prefix=OFFERS %s
# RUN: llvm-objdump -d a.dll | FileCheck --check-prefix=LOCAL %s
# OFFERS-NOT: Name: _ZTS1V
# OFFERS:     Name: _ZTI1V
# OFFERS-NOT: Name: _ZTS1V
# LOCAL:      <fa>:
# LOCAL-NEXT: leaq

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj a.lib \
# RUN:   -out:main.exe -map:main.map -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=PREEMPT %s
# RUN: cat main.map > main.txt
# RUN: llvm-readobj --coff-imports main.exe >> main.txt
# RUN: FileCheck --check-prefix=SLOTS --input-file=main.txt %s
# PREEMPT: Preempting typeinfo for V in main.obj with its instance in a.dll
## Two runs of slots, for the base pointer in _ZTI1W and the vtable's RTTI
## word.
# SLOTS:      _ZTI1W [[#%x,W:]] main.obj
# SLOTS:      _ZTV1V [[#%x,VT:]] main.obj
# SLOTS:      Symbol: fa (1)
# SLOTS:      ImportAddressTableRVA: 0x[[#%X,W+16-0x140000000]]
# SLOTS-NEXT: Symbol: _ZTI1V (0)
# SLOTS:      ImportAddressTableRVA: 0x[[#%X,VT+8-0x140000000]]
# SLOTS-NEXT: Symbol: _ZTI1V (0)

## Two DLLs that both keep the object, which no import path joins, are not
## reported: the executable takes the first one's offer, which its table names
## for the second's fill, and each DLL's fill has the object's name word to
## write.
# RUN: lld-link -import-slots -dll -noentry b.obj crt.obj ntdll.lib -out:b.dll \
# RUN:   -map:b.map
# RUN: lld-link -import-slots -entry:main -subsystem:console twice.obj a.lib \
# RUN:   b.lib -out:twice.exe -map:twice.map -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=TWICE %s
# RUN: FileCheck --check-prefix=FILL --input-file=a.map %s
# RUN: FileCheck --check-prefix=FILL --input-file=b.map %s
# RUN: FileCheck --check-prefix=TABLE --input-file=twice.map %s
# TWICE-NOT: error:
# TWICE:     Preempting typeinfo for V in twice.obj with its instance in a.dll
# TWICE-NOT: which owns it for the program
# TWICE-NOT: error:
# FILL:      __llvm_fill_list
## The table's check, live only where the executable names an owner.
# TABLE:     __ovr_check

#--- a.s
  .linkbindings
  .section .rdata,"dr",discard,_ZTI1V
  .globl _ZTI1V
  .p2align 3
_ZTI1V:
  .linkbinding _ZTI1V, default
  .quad __class_type_info+16
  .quad _ZTS1V-9223372036854775808
  .section .rdata,"dr",discard,_ZTS1V
  .globl _ZTS1V
_ZTS1V:
  .linkbinding _ZTS1V, default
  .asciz "1V"

  .section .rdata,"dr"
__class_type_info:
  .quad 0, 0, 0

  .text
  .globl fa
fa:
  movq __imp__ZTI1V(%rip), %rax
  retq
  .section .drectve,"yni"
  .ascii " -export:fa"

#--- b.s
  .linkbindings
  .section .rdata,"dr",discard,_ZTI1V
  .globl _ZTI1V
  .p2align 3
_ZTI1V:
  .linkbinding _ZTI1V, default
  .quad __class_type_info+16
  .quad _ZTS1V-9223372036854775808
  .section .rdata,"dr",discard,_ZTS1V
  .globl _ZTS1V
_ZTS1V:
  .linkbinding _ZTS1V, default
  .asciz "1V"

  .section .rdata,"dr"
__class_type_info:
  .quad 0, 0, 0

  .text
  .globl fb
fb:
  movq __imp__ZTI1V(%rip), %rax
  retq
  .section .drectve,"yni"
  .ascii " -export:fb"

#--- main.s
  .linkbindings
  .section .rdata,"dr",discard,_ZTI1V
  .globl _ZTI1V
  .p2align 3
_ZTI1V:
  .linkbinding _ZTI1V, default
  .quad __class_type_info+16
  .quad _ZTS1V-9223372036854775808
  .section .rdata,"dr",discard,_ZTS1V
  .globl _ZTS1V
_ZTS1V:
  .linkbinding _ZTS1V, default
  .asciz "1V"

## A type_info object that only the executable keeps, whose base is V.
  .section .rdata,"dr",discard,_ZTI1W
  .globl _ZTI1W
  .p2align 3
_ZTI1W:
  .linkbinding _ZTI1W, default
  .quad __si_class_type_info+16
  .quad _ZTS1W-9223372036854775808
  .quad _ZTI1V
  .section .rdata,"dr",discard,_ZTS1W
  .globl _ZTS1W
_ZTS1W:
  .linkbinding _ZTS1W, default
  .asciz "1W"

## A vtable of a class with no key function, which the executable keeps.
  .section .rdata,"dr",discard,_ZTV1V
  .globl _ZTV1V
  .p2align 3
_ZTV1V:
  .linkbinding _ZTV1V, default
  .quad 0
  .quad _ZTI1V
  .quad vf

  .section .rdata,"dr"
__class_type_info:
  .quad 0, 0, 0
__si_class_type_info:
  .quad 0, 0, 0

  .text
vf:
  retq
  .globl main
main:
  callq *__imp_fa(%rip)
  movq __imp__ZTI1W(%rip), %rax
  leaq _ZTV1V+16(%rip), %rax
  retq

#--- twice.s
  .linkbindings
  .section .rdata,"dr",discard,_ZTI1V
  .globl _ZTI1V
  .p2align 3
_ZTI1V:
  .linkbinding _ZTI1V, default
  .quad __class_type_info+16
  .quad _ZTS1V-9223372036854775808
  .section .rdata,"dr",discard,_ZTS1V
  .globl _ZTS1V
_ZTS1V:
  .linkbinding _ZTS1V, default
  .asciz "1V"

  .section .rdata,"dr"
__class_type_info:
  .quad 0, 0, 0
  .data
  .globl vptr
vptr:
  .quad _ZTI1V

  .text
  .globl main
main:
  callq *__imp_fa(%rip)
  callq *__imp_fb(%rip)
  movq vptr(%rip), %rax
  retq

## A stand-in for the runtime's check of the override table.
  .data
  .globl __ovr_check
__ovr_check:
  .quad 0

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0

## A stand-in for the builtins' __llvm_fill.
  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  leaq __llvm_fill_list(%rip), %rax
  retq
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
