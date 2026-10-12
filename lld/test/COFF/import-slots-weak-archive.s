# REQUIRES: x86
## A weak external reference to X does not, by itself, pull a static-library
## member that defines X, as a weak reference does not on ELF. But a reference
## other than the weak external must pull it, which COFF's weak external
## otherwise prevents by consuming the symbol: link.exe, and this linker
## without -import-slots, resolve X to the weak fallback and leave the member
## unloaded, where ELF loads it for the strong reference. Under -import-slots
## the member loads.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium weak.s -o weak.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium strong.s -o strong.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium def.s -o def.obj
# RUN: llvm-lib -out:libdef.lib def.obj

## A weak reference plus a strong reference: -import-slots loads the member.
# RUN: lld-link -dll -noentry -import-slots weak.obj strong.obj libdef.lib \
# RUN:   -out:slots.dll -lldmap:slots.map
# RUN: FileCheck --check-prefix=PULLED %s < slots.map

## The strong reference seen before the weak external: still loaded.
# RUN: lld-link -dll -noentry -import-slots strong.obj weak.obj libdef.lib \
# RUN:   -out:order.dll -lldmap:order.map
# RUN: FileCheck --check-prefix=PULLED %s < order.map

## Without -import-slots the member is not loaded (link.exe parity): the weak
## external resolves X and the strong reference binds to that fallback.
# RUN: lld-link -dll -noentry weak.obj strong.obj libdef.lib \
# RUN:   -out:plain.dll -lldmap:plain.map
# RUN: FileCheck --check-prefix=NOTPULLED %s < plain.map

## Only the weak reference: the member is not loaded, even under -import-slots,
## as a weak reference does not pull on ELF.
# RUN: lld-link -dll -noentry -import-slots weak.obj libdef.lib \
# RUN:   -out:weakonly.dll -lldmap:weakonly.map
# RUN: FileCheck --check-prefix=NOTPULLED %s < weakonly.map

# PULLED: def.obj
# NOTPULLED-NOT: def.obj

#--- weak.s
        .text
        .globl geta
geta:
        leaq weakp(%rip), %rax
        retq
        .data
        .globl weakp
weakp:
        .quad X
        .weak X

#--- strong.s
        .text
        .globl useX
useX:
        jmp X

#--- def.s
        .text
        .globl X
X:
        movl $1, %eax
        retq
