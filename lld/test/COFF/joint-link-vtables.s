# REQUIRES: x86
## The vtables of a joint link. One static library's object, linked into two
## member DLLs, defines a strong vtable and type_info that both export: one
## member keeps them for the others, which import them, and their exports
## forward there. A vtable whose exact casts compare vtable pointers (named by
## /exactcast:) is kept once too, exported under a name no import library
## holds, so that the compare decides for every member's objects; a member
## that only casts imports it. Every other vtable copy stays in each member.
## Code that took a dropped vtable's address point now loads it from the
## import of the name the owner exports for it; and a member imports the run of
## each class it imports from the member that has the class's vtable, which
## that member exports, even where it keeps no live copy of it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium k.s -o k.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium app.s -o app.obj
# RUN: llvm-lib -out:k.lib k.obj

# RUN: lld-link -import-slots -dll -noentry a.obj k.lib -include:newK \
# RUN:   -out:a.dll -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj k.lib -include:newK \
# RUN:   -out:b.dll -lldjointscan:b.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan app.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj k.lib -include:newK \
# RUN:   -out:a.dll -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj k.lib -include:newK \
# RUN:   -out:b.dll -lldjoint:facts/b.dll.jfacts
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:app.exe -lldjoint:facts/app.exe.jfacts

## a.dll, declared first, keeps K's vtable and type_info and D's vtable, which
## it exports privately, and the runs of K and D.
# RUN: llvm-readobj --coff-exports a.dll | FileCheck --check-prefix=A %s
# RUN: llvm-nm a.lib | FileCheck --check-prefix=ALIB %s
# A-DAG: Name: _ZTI1K
# A-DAG: Name: _ZTV1D
# A-DAG: Name: _ZTV1D$ap16
# A-DAG: Name: _ZTV1K
# A-DAG: Name: _ZTV1K$ap16
# A-DAG: Name: __llvm_vtr$[[D:.*]]e
# A-DAG: Name: __llvm_vtr$[[D]]m
# A-DAG: Name: __llvm_vtr$[[K:.*]]e
# A-DAG: Name: __llvm_vtr$[[K]]m
# ALIB:     __imp__ZTV1K
# ALIB-NOT: __imp__ZTV1D

## b.dll imports what its code reads, a.dll's identity, which covers the names
## it exports for b.dll alone, and the three words of the run of each class
## its checks read, consecutive; its exports of K's forward, and it exports
## nothing of D's.
# RUN: llvm-readobj --coff-imports --coff-exports b.dll > b.txt
# RUN: llvm-objdump -d b.dll >> b.txt
# RUN: FileCheck --check-prefix=B %s < b.txt
# B:      Name: a.dll
# B-NEXT: ImportLookupTableRVA:
# B-NEXT: ImportAddressTableRVA: 0x[[#%X,IAT:]]
# B-NEXT: Symbol: _ZTI1K (0)
# B-NEXT: Symbol: _ZTV1D$ap16 (0)
# B-NEXT: Symbol: _ZTV1K (0)
# B-NEXT: Symbol: _ZTV1K$ap16 (0)
# B-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# B-NEXT: Symbol: _ZTV1D$ap16 (0)
# B-NEXT: Symbol: __llvm_vtr$[[BD:.+]]e (0)
# B-NEXT: Symbol: __llvm_vtr$[[BD]]m (0)
# B-NEXT: Symbol: _ZTV1K$ap16 (0)
# B-NEXT: Symbol: __llvm_vtr$[[BK:.+]]e (0)
# B-NEXT: Symbol: __llvm_vtr$[[BK]]m (0)
# B-NEXT: }
# B:      Name: _ZTI1K
# B-NEXT: ForwardedTo: a._ZTI1K
# B-NOT:  Name: _ZTV1D
# B:      Name: _ZTV1K
# B-NEXT: ForwardedTo: a._ZTV1K
# B:      Name: _ZTV1K$ap16
# B-NEXT: ForwardedTo: a._ZTV1K$ap16

## b.dll's constructions of D and K load the address point a.dll exports, and
## its checks read the runs' words; C's copy stays.
# B:      <bf>:
# B-NEXT: movq {{.*}} # 0x[[#%x,IAT+6442450944+8]]
# B-NEXT: leaq
# B-NEXT: movq {{.*}} # 0x[[#%x,IAT+6442450944+40]]
# B-NEXT: movq {{.*}} # 0x[[#%x,IAT+6442450944+64]]
# B:      retq
# B:      movq {{.*}} # 0x[[#%x,IAT+6442450944+24]]

## The executable only casts to D, and imports a.dll's vtable for its compare,
## with the identity that covers it.
# RUN: llvm-readobj --coff-imports app.exe | FileCheck --check-prefix=APP %s
# APP:      Name: a.dll
# APP-NEXT: ImportLookupTableRVA:
# APP-NEXT: ImportAddressTableRVA:
# APP-NEXT: Symbol: _ZTV1D (0)
# APP-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# APP-NEXT: }

#--- k.s
  .section .rdata,"dr",one_only,_ZTV1K
  .globl _ZTV1K
  .p2align 6
_ZTV1K:
  .quad 0
  .quad _ZTI1K
  .quad kf
  .linkvtable _ZTV1K, 16, "1K", ""
  .globl _ZTV1K$ap16
_ZTV1K$ap16 = _ZTV1K+16

  .section .rdata,"dr",one_only,_ZTI1K
  .globl _ZTI1K
_ZTI1K:
  .quad 0

  .section .text,"xr",one_only,kf
  .globl kf
kf:
  retq

  .section .text,"xr",one_only,newK
  .globl newK
newK:
  leaq _ZTV1K+16(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:_ZTV1K,DATA -export:_ZTV1K$ap16,DATA -export:_ZTI1K,DATA"

#--- a.s
  .section .rdata,"dr",discard,_ZTV1D
  .globl _ZTV1D
  .p2align 6
_ZTV1D:
  .quad 0
  .quad 0
  .quad af
  .linkvtable _ZTV1D, 16, "1D", ""
  .globl _ZTV1D$ap16
_ZTV1D$ap16 = _ZTV1D+16

  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 6
_ZTV1C:
  .quad 0
  .quad 0
  .quad af
  .linkvtable _ZTV1C, 16, "1C", ""
  .globl _ZTV1C$ap16
_ZTV1C$ap16 = _ZTV1C+16

  .text
  .globl af
af:
  leaq _ZTV1D+16(%rip), %rax
  leaq _ZTV1C+16(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:af /exactcast:_ZTV1D"

#--- b.s
  .section .rdata,"dr",discard,_ZTV1D
  .globl _ZTV1D
  .p2align 6
_ZTV1D:
  .quad 0
  .quad 0
  .quad bf
  .linkvtable _ZTV1D, 16, "1D", ""
  .globl _ZTV1D$ap16
_ZTV1D$ap16 = _ZTV1D+16

  .section .rdata,"dr",discard,_ZTV1C
  .globl _ZTV1C
  .p2align 6
_ZTV1C:
  .quad 0
  .quad 0
  .quad bf
  .linkvtable _ZTV1C, 16, "1C", ""
  .globl _ZTV1C$ap16
_ZTV1C$ap16 = _ZTV1C+16

  .text
  .globl bf
bf:
  leaq _ZTV1D+16(%rip), %rax
  leaq _ZTV1C+16(%rip), %rax
  movq __llvm_vtables_owner$1D(%rip), %rax
  movq __llvm_vtables_owner$1K(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:bf /exactcast:_ZTV1D"

#--- app.s
  .text
  .globl main
main:
  movq __imp__ZTV1D(%rip), %rax
  retq
  .weak _ZTV1D
