# REQUIRES: x86

## Under -guard:cf, where the mismatch is bounded by the
## image and no foreign object references an import, foreign code can hand
## ours only functions in the image, which the bound accepts. A type that
## only a C declaration resolving to a definition without a prefix in the
## image, __kcfi_inflow_, or a reference from a foreign object to a definition
## of ours, __kcfi_param_, opens dynamically is then open statically, and so
## is one that only a __kcfi_tinflow_ fact of such a type opens. Its routine
## points R10 at its list and jumps to the static scanner, which the bound
## sends to the dynamic scanner for a target in the image; the static
## scanner, which nothing else refers to, is kept for it. Here
## __kcfi_inflow_0000000022222222_getter and __kcfi_param_0000000022222222_ours open
## 0x22222222, which opens 0x33333333.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc user.s -o user.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc getter.s -o getter.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc getter-imp.s \
# RUN:   -o getter-imp.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc caller.s -o caller.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: llvm-dlltool -m i386:x86-64 -d getter.def -D getter.dll -l getter.lib
# RUN: lld-link main.obj user.obj getter.obj -guard:cf \
# RUN:   -entry:main -include:user -debug:symtab -opt:ref -out:inflow.exe
# RUN: llvm-objdump -d inflow.exe | FileCheck %s
# RUN: lld-link main.obj caller.obj -guard:cf -entry:main \
# RUN:   -include:caller -debug:symtab -opt:ref -out:param.exe
# RUN: llvm-objdump -d param.exe | FileCheck %s

# CHECK:      <__llvm_kcfi_mismatch_22222222>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp {{.*}} <__llvm_kcfi_open>
# CHECK-EMPTY:
# CHECK-NEXT: <__llvm_kcfi_mismatch_33333333>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp {{.*}} <__llvm_kcfi_open>
# CHECK:      <__llvm_kcfi_open>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r11 # 0x140000000
# CHECK-NEXT:   cmpq %r11, %rax
# CHECK-NEXT:   jb [[SCANNER:0x[0-9a-f]+]]
# CHECK-NEXT:   leaq {{.*}}(%rip), %r11
# CHECK-NEXT:   cmpq %r11, %rax
# CHECK-NEXT:   jae [[SCANNER]]
# CHECK-NEXT:   jmp {{.*}} <__llvm_kcfi_open_dynamic>

## Where a foreign object references an import, which can hand on a pointer
## that a DLL gives it at run time, where the declaration resolves to an
## import, and where nothing bounds the mismatch, both types stay open
## dynamically.
# RUN: lld-link main.obj user.obj getter-imp.obj lib.lib \
# RUN:   -guard:cf -entry:main -include:user -debug:symtab -opt:ref \
# RUN:   -out:imp.exe
# RUN: llvm-objdump -d imp.exe | FileCheck %s --check-prefix=DYNAMIC
# RUN: lld-link main.obj user.obj getter.lib plain.obj \
# RUN:   -guard:cf -entry:main -include:user -include:plain -debug:symtab \
# RUN:   -opt:ref -out:dll.exe
# RUN: llvm-objdump -d dll.exe | FileCheck %s --check-prefix=DYNAMIC
# RUN: lld-link main.obj user.obj getter.obj -entry:main \
# RUN:   -include:user -debug:symtab -opt:ref -out:noguard.exe
# RUN: llvm-objdump -d noguard.exe | FileCheck %s --check-prefix=DYNAMIC

# DYNAMIC:      <__llvm_kcfi_mismatch_22222222>:
# DYNAMIC-NEXT:   leaq {{.*}}(%rip), %r10
# DYNAMIC-NEXT:   jmp {{.*}} <__llvm_kcfi_open_dynamic>
# DYNAMIC-NEXT:   int3
# DYNAMIC-EMPTY:
# DYNAMIC-NEXT: <__llvm_kcfi_mismatch_33333333>:
# DYNAMIC-NEXT:   leaq {{.*}}(%rip), %r10
# DYNAMIC-NEXT:   jmp {{.*}} <__llvm_kcfi_open_dynamic>
# DYNAMIC-NEXT:   int3

#--- lib.def
LIBRARY lib.dll
EXPORTS
imported

#--- getter.def
LIBRARY getter.dll
EXPORTS
getter

#--- getter.s
## Foreign code in the image, with a function of its own that it can hand
## out.
        .text
        .globl getter
getter:
        leaq cb(%rip), %rax
        retq
        .def cb; .scl 3; .type 32; .endef
cb:
        retq

#--- getter-imp.s
        .text
        .globl getter
getter:
        leaq imported(%rip), %rax
        retq

#--- caller.s
## Foreign code that calls a definition of ours.
        .text
        .globl caller
caller:
        leaq cb(%rip), %rcx
        jmp ours
        .def cb; .scl 3; .type 32; .endef
cb:
        retq

#--- plain.s
        .text
        .globl plain
plain:
        retq

        .data
        .quad plain

#--- user.s
## Our object, which calls getter.
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def user; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,user
        .p2align 4
        .fill 4, 1, 0x90
__cfi_user:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl user
user:
        jmp getter

        .weak __kcfi_inflow_0000000022222222_getter
__kcfi_inflow_0000000022222222_getter = 0x22222222

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
        callq __llvm_kcfi_dispatch_33333333
        retq

        .def ours; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,ours
        .p2align 4
        .fill 4, 1, 0x90
__cfi_ours:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl ours
ours:
        retq

        .weak __kcfi_param_0000000022222222_ours
__kcfi_param_0000000022222222_ours = 0x22222222
        .weak __kcfi_tinflow_0000000033333333_0000000022222222
__kcfi_tinflow_0000000033333333_0000000022222222 = 0x33333333

        .irpc d, 23
        .weak __llvm_kcfi_mismatch_\d\d\d\d\d\d\d\d
__llvm_kcfi_mismatch_\d\d\d\d\d\d\d\d = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_\d\d\d\d\d\d\d\d
        .globl __llvm_kcfi_dispatch_\d\d\d\d\d\d\d\d
        .p2align 4
__llvm_kcfi_dispatch_\d\d\d\d\d\d\d\d:
        cmpl $0x\d\d\d\d\d\d\d\d, -4(%rax)
        jne __llvm_kcfi_mismatch_\d\d\d\d\d\d\d\d
        jmpq *%rax
        .endr

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
