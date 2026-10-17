# REQUIRES: x86
## Under -import-slots, an x86-64 object built with /guard:cf (@feat.00 bit
## 0x800) lists every function-typed address-take in .gfids$y, so a REL32 that
## it does not describe, against a function-typed symbol the table does not
## list, is a branch: it takes the import thunk with no warning, and the
## function's word in static data stays an in-place slot. A listed symbol, and
## one not typed as a function, keep the conservative treatment, the thunk as
## the function's address with a warning, which /ignore:undescribedaddress
## silences.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc guarded.s -o guarded.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc unguarded.s -o unguarded.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64

# RUN: lld-link -import-slots -entry:main -subsystem:console guarded.obj \
# RUN:   a.lib -out:guarded.exe 2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-imports guarded.exe | FileCheck --check-prefix=IMPORTS %s
# WARN-NOT:   may take the address of call
# WARN:       warning: guarded.obj: may take the address of taken, imported from a.dll, in an instruction it does not describe
# WARN-NEXT:  warning: guarded.obj: may take the address of asdata, imported from a.dll, in an instruction it does not describe
# WARN-NOT:   warning

## The branch target's word in static data is an in-place slot.
# IMPORTS:      Name: a.dll
# IMPORTS:      ImportAddressTableRVA: 0x3000
# IMPORTS-NEXT: Symbol: call (1)

## Without guard metadata every such reference is an address-take.
# RUN: lld-link -import-slots -entry:main -subsystem:console unguarded.obj \
# RUN:   a.lib -out:unguarded.exe 2>&1 | FileCheck --check-prefix=UNGUARDED %s
# UNGUARDED: warning: unguarded.obj: may take the address of call, imported from a.dll

# RUN: lld-link -import-slots -entry:main -subsystem:console guarded.obj \
# RUN:   a.lib -out:quiet.exe -ignore:undescribedaddress 2>&1 | count 0

#--- a.def
LIBRARY a.dll
EXPORTS
  call
  taken
  asdata

#--- guarded.s
        .def @feat.00
        .scl 3
        .type 0
        .endef
        .globl @feat.00
.set @feat.00, 0x800
        .def call
        .scl 2
        .type 32
        .endef
        .def taken
        .scl 2
        .type 32
        .endef
        .text
        .globl main
main:
        callq call
        leaq taken(%rip), %rax
        leaq asdata(%rip), %rcx
        retq
        .data
        .quad call
        .section .gfids$y,"dr"
        .symidx taken

#--- unguarded.s
        .def call
        .scl 2
        .type 32
        .endef
        .text
        .globl main
main:
        callq call
        retq
