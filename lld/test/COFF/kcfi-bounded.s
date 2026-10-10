# REQUIRES: x86

## Under -guard:cf, in an image whose guard function table
## lists a function without a KCFI prefix that a foreign object, one with code
## and no prefix anywhere, lists, here plain, which such an object without
## guard metadata refers to, the routine of a closed type sends a target
## outside the image to the trap. It points R10 at an empty list and jumps to
## the dynamic scanner with any other target, which the dispatch routine finds
## in RAX and the check routine in RCX. The dynamic scanner, which nothing
## else refers to, is kept for them.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc thunk.s -o thunk.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc filter.s -o filter.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc init.s -o init.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc table.s -o table.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc quiet.s -o quiet.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: lld-link main.obj plain.obj -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_dispatch_22222222>:
# CHECK:        jne 0x140001071 <__llvm_kcfi_mismatch_22222222>
# CHECK:      <__llvm_kcfi_check_22222222>:
# CHECK:        jne 0x14000109d <__llvm_kcfi_check_mismatch_22222222>
# CHECK:      <__llvm_kcfi_trap>:
# CHECK:      <__llvm_kcfi_open_dynamic>:
# CHECK-NEXT:   ud2
# CHECK:      <__llvm_kcfi_check_open_dynamic>:
# CHECK-NEXT:   nop
# CHECK-NEXT:   ud2
# CHECK:      <__llvm_kcfi_mismatch_22222222>:
# CHECK-NEXT:   leaq -0x1078(%rip), %r11 # 0x140000000
# CHECK-NEXT:   cmpq %r11, %rax
# CHECK-NEXT:   jb 0x140001040 <__llvm_kcfi_trap>
# CHECK-NEXT:   leaq 0x3f78(%rip), %r11 # 0x140005000
# CHECK-NEXT:   cmpq %r11, %rax
# CHECK-NEXT:   jae 0x140001040 <__llvm_kcfi_trap>
# CHECK-NEXT:   leaq 0x1090(%rip), %r10 # 0x140002128
# CHECK-NEXT:   jmp 0x140001050 <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_check_mismatch_22222222>:
# CHECK-NEXT:   leaq -0x10a4(%rip), %r11 # 0x140000000
# CHECK-NEXT:   cmpq %r11, %rcx
# CHECK-NEXT:   jb 0x140001040 <__llvm_kcfi_trap>
# CHECK-NEXT:   leaq 0x3f4c(%rip), %r11 # 0x140005000
# CHECK-NEXT:   cmpq %r11, %rcx
# CHECK-NEXT:   jae 0x140001040 <__llvm_kcfi_trap>
# CHECK-NEXT:   leaq 0x1064(%rip), %r10 # 0x140002128
# CHECK-NEXT:   jmp 0x140001060 <__llvm_kcfi_check_open_dynamic>

## The empty list: its head, then the odd word that ends it.
# DATA: 140002120 00000000 00000000 01000000 00000000

## An import thunk that a foreign object lists counts too, as foreign code can
## take an import's address, and so does a local function of a foreign object
## that defines no external function, only a table that holds it.
# RUN: lld-link main.obj thunk.obj lib.lib -guard:cf \
# RUN:   -entry:main -debug:symtab -opt:ref -out:thunk.exe
# RUN: llvm-objdump -d thunk.exe | FileCheck %s --check-prefix=BOUND
# RUN: lld-link main.obj table.obj -guard:cf -entry:main \
# RUN:   -include:ops -debug:symtab -opt:ref -out:table.exe
# RUN: llvm-objdump -d table.exe | FileCheck %s --check-prefix=BOUND

# BOUND:      <__llvm_kcfi_mismatch_22222222>:
# BOUND-NEXT:   leaq {{.*}}(%rip), %r11 # 0x140000000
# BOUND:      <__llvm_kcfi_check_mismatch_22222222>:
# BOUND-NEXT:   leaq {{.*}}(%rip), %r11 # 0x140000000

## Where the table lists only functions with a prefix, or a function without
## one only from an object with a prefix, such as a filter of a structured
## exception handler, or a static initializer whose object keeps its prefixes
## only in sections the link drops, or a foreign object refers to no function,
## or there is no table, both routines stay the trap, and the dynamic scanner
## is not kept.
# RUN: lld-link main.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -opt:ref -out:prefixed.exe
# RUN: llvm-objdump -d prefixed.exe | FileCheck %s --check-prefix=TRAP
# RUN: lld-link main.obj filter.obj -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:filter.exe
# RUN: llvm-objdump -d filter.exe | FileCheck %s --check-prefix=TRAP
# RUN: lld-link main.obj init.obj -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:init.exe
# RUN: llvm-objdump -d init.exe | FileCheck %s --check-prefix=TRAP
# RUN: lld-link main.obj quiet.obj -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:quiet.exe
# RUN: llvm-objdump -d quiet.exe | FileCheck %s --check-prefix=TRAP
# RUN: lld-link main.obj plain.obj -entry:main -debug:symtab \
# RUN:   -opt:ref -out:noguard.exe
# RUN: llvm-objdump -d noguard.exe | FileCheck %s --check-prefix=TRAP

# TRAP:      <__llvm_kcfi_dispatch_22222222>:
# TRAP:        jne {{.*}} <__llvm_kcfi_trap>
# TRAP:      <__llvm_kcfi_check_22222222>:
# TRAP:        jne {{.*}} <__llvm_kcfi_trap>
# TRAP-NOT:  <__llvm_kcfi_mismatch_22222222>:
# TRAP-NOT:  <__llvm_kcfi_open_dynamic>:

#--- lib.def
LIBRARY lib.dll
EXPORTS
imported

#--- thunk.s
        .globl @feat.00
@feat.00 = 0x800

        .def foreign; .scl 2; .type 32; .endef
        .text
        .globl foreign
foreign:
        leaq imported(%rip), %rax
        retq

        .section .gfids$y,"dr"
        .symidx imported

#--- filter.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def ours; .scl 2; .type 32; .endef
        .text
        .p2align 4
        .fill 4, 1, 0x90
__cfi_ours:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl ours
ours:
        retq

        .def filter; .scl 3; .type 32; .endef
filter:
        retq

        .data
        .quad ours
        .quad filter

        .section .gfids$y,"dr"
        .symidx ours
        .symidx filter

#--- init.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def init; .scl 3; .type 32; .endef
        .text
init:
        retq

        .def unused; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,unused
        .p2align 4
        .fill 4, 1, 0x90
__cfi_unused:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl unused
unused:
        retq

        .section .CRT$XCU,"dr"
        .quad init

        .section .gfids$y,"dr"
        .symidx init

#--- table.s
        .globl @feat.00
@feat.00 = 0x800

        .def cb; .scl 3; .type 32; .endef
        .text
cb:
        retq

        .data
        .globl ops
ops:
        .quad cb

        .section .gfids$y,"dr"
        .symidx cb

#--- quiet.s
        .def quiet; .scl 2; .type 32; .endef
        .text
        .globl quiet
quiet:
        retq

#--- plain.s
        .def plain; .scl 2; .type 32; .endef
        .text
        .globl plain
plain:
        retq

        .data
        .quad plain

#--- main.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .fill 4, 1, 0x90
__cfi_main:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl main
main:
        callq __llvm_kcfi_dispatch_22222222
        callq __llvm_kcfi_check_22222222
        retq

        .weak __llvm_kcfi_mismatch_22222222
__llvm_kcfi_mismatch_22222222 = __llvm_kcfi_trap
        .weak __llvm_kcfi_check_mismatch_22222222
__llvm_kcfi_check_mismatch_22222222 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_22222222
        .globl __llvm_kcfi_dispatch_22222222
        .p2align 4
__llvm_kcfi_dispatch_22222222:
        cmpl $0x22222222, -4(%rax)
        jne __llvm_kcfi_mismatch_22222222
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_check_22222222
        .globl __llvm_kcfi_check_22222222
        .p2align 4
__llvm_kcfi_check_22222222:
        cmpl $0x22222222, -4(%rcx)
        jne __llvm_kcfi_check_mismatch_22222222
        retq

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        movl $64, %ecx
        int $0x29

        .section .text,"xr",discard,__llvm_kcfi_open
        .globl __llvm_kcfi_open
        .p2align 4
__llvm_kcfi_open:
        int3

        .section .text,"xr",discard,__llvm_kcfi_open_dynamic
        .globl __llvm_kcfi_open_dynamic
        .p2align 4
__llvm_kcfi_open_dynamic:
        ud2

        .section .text,"xr",discard,__llvm_kcfi_check_open
        .globl __llvm_kcfi_check_open
        .p2align 4
__llvm_kcfi_check_open:
        nop
        int3

        .section .text,"xr",discard,__llvm_kcfi_check_open_dynamic
        .globl __llvm_kcfi_check_open_dynamic
        .p2align 4
__llvm_kcfi_check_open_dynamic:
        nop
        ud2

        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0
