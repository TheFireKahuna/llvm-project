# REQUIRES: x86
## A default the standard specifies as calling another runs that other's
## replacement: under -import-slots the linker forwards an unreplaced slot to
## the nearest replaced slot its chain reaches, naming the replacement's body,
## or a thunk to it when a step drops the default's second argument. A forwarded
## slot holds its own tag and the target's RVA, has an anchor entry, but sets no
## mask bit, which marks a replacement rather than a forwarding.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium records.s -o records.obj
# RUN: lld-link -import-slots -dll -noentry records.obj -out:records.dll \
# RUN:   -implib:records.lib

## Replacing operator new forwards operator new[] to it directly: slot 1 holds
## tag 0xC5E1A7F2 and operator new's RVA, the same body slot 0 names.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium new.s -o new.obj
# RUN: lld-link -import-slots new.obj records.lib -out:new.exe -entry:_Znwy \
# RUN:   -subsystem:console
# RUN: od -A x -t x1 -j 0x40 -N 0x10 new.exe | FileCheck --check-prefix=NEW %s
# NEW: 000040 f3 a7 e1 c5 00 10 00 00 f2 a7 e1 c5 00 10 00 00
## Only slot 0 is a replacement, so only its mask bit is set.
# RUN: llvm-objdump -s --section=.ovra new.exe | FileCheck --check-prefix=NEWMASK %s
# NEWMASK: {{[0-9a-f]+}} d5c23a7b 01000800 01000000 00000000

## Replacing the aligned deallocation forwards the sized aligned one to it
## through a thunk that drops the size: slot 9 names the thunk, which moves the
## third argument into the second and jumps to the body.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium del.s -o del.obj
# RUN: lld-link -import-slots del.obj records.lib -out:del.exe \
# RUN:   -entry:_ZdlPvSt11align_val_t -subsystem:console
# RUN: llvm-objdump -d --no-show-raw-insn del.exe | FileCheck --check-prefix=THUNK %s
# THUNK:      movq %r8, %rdx
# THUNK-NEXT: jmp
## The sized aligned slot is forwarded, not replaced, so only slot 7's mask bit
## is set (0x80).
# RUN: llvm-objdump -s --section=.ovra del.exe | FileCheck --check-prefix=DELMASK %s
# DELMASK: {{[0-9a-f]+}} d5c23a7b 01000a00 80000000 00000000

#--- records.s
## Stands in for the C++ runtime, whose defaults record their slots and the
## defaults they forward to.
        .text
        .weak _Znwy
_Znwy:
        retq
        .weak _Znay
_Znay:
        retq
        .weak _ZdlPvSt11align_val_t
_ZdlPvSt11align_val_t:
        retq
        .weak _ZdlPvySt11align_val_t
_ZdlPvySt11align_val_t:
        retq
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /OVERRIDABLE:_Znay,1,0"
        .ascii " /OVERRIDABLE:_ZdlPvSt11align_val_t,7"
        .ascii " /OVERRIDABLE:_ZdlPvySt11align_val_t,9,7,1"
        .ascii " /EXPORT:_Znwy /EXPORT:_Znay /EXPORT:_ZdlPvSt11align_val_t"
        .ascii " /EXPORT:_ZdlPvySt11align_val_t"

#--- new.s
        .text
        .globl _Znwy
_Znwy:
        retq
        .data
        .globl __ovr_check
__ovr_check:
        .quad 0
        .section .drectve,"yn"
        .ascii " /OVERRIDABLE:_Znwy /INCLUDE:__llvm_overrides_v1"

#--- del.s
        .text
        .globl _ZdlPvSt11align_val_t
_ZdlPvSt11align_val_t:
        retq
        .data
        .globl __ovr_check
__ovr_check:
        .quad 0
        .section .drectve,"yn"
        .ascii " /OVERRIDABLE:_ZdlPvSt11align_val_t /INCLUDE:__llvm_overrides_v1"
