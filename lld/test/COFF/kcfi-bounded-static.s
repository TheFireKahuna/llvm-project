# REQUIRES: x86

## Under -guard:cf, in an image whose guard function table
## lists a function without a KCFI prefix, here plain, the static scanner is
## entered through a routine that gives it a target outside the image, and the
## dynamic scanner any other, with the list in R10 that each statically open
## type's routine points it at. A compiled routine jumps to the scanner, as
## the one the linker makes does, through its symbol.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: lld-link main.obj plain.obj lib.lib -guard:cf \
# RUN:   -entry:main -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s

# CHECK:      <__llvm_kcfi_dispatch_11111111>:
# CHECK:        jne 0x140001081 <__llvm_kcfi_mismatch_11111111>
# CHECK:      <__llvm_kcfi_mismatch_33333333>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10 # 0x140002138 <__llvm_kcfi_list_33333333+0x8>
# CHECK-NEXT:   jmp 0x14000108d <__llvm_kcfi_open>
# CHECK:      <static_scanner>:
# CHECK-NEXT:   int3
# CHECK:      <__llvm_kcfi_open_dynamic>:
# CHECK-NEXT:   ud2
# CHECK:      <__llvm_kcfi_mismatch_11111111>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10 # 0x140002120
# CHECK-NEXT:   jmp 0x14000108d <__llvm_kcfi_open>
# CHECK-EMPTY:
# CHECK-NEXT: <__llvm_kcfi_open>:
# CHECK-NEXT:   leaq -0x1094(%rip), %r11 # 0x140000000
# CHECK-NEXT:   cmpq %r11, %rax
# CHECK-NEXT:   jb 0x140001060 <static_scanner>
# CHECK-NEXT:   leaq 0x3f5c(%rip), %r11 # 0x140005000
# CHECK-NEXT:   cmpq %r11, %rax
# CHECK-NEXT:   jae 0x140001060 <static_scanner>
# CHECK-NEXT:   jmp 0x140001070 <__llvm_kcfi_open_dynamic>

## Where the table lists only functions with a prefix, the routines jump to the
## static scanner directly.
# RUN: lld-link main.obj lib.lib -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:prefixed.exe
# RUN: llvm-objdump -d prefixed.exe | FileCheck %s --check-prefix=DIRECT

# DIRECT:      <__llvm_kcfi_mismatch_33333333>:
# DIRECT-NEXT:   leaq
# DIRECT-NEXT:   jmp 0x140001060 <static_scanner>
# DIRECT:      <__llvm_kcfi_mismatch_11111111>:
# DIRECT-NEXT:   leaq
# DIRECT-NEXT:   jmp 0x140001060 <static_scanner>
# DIRECT-NOT:  <__llvm_kcfi_open_dynamic>:

#--- lib.def
LIBRARY lib.dll
EXPORTS
imported

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
        movq __imp_imported(%rip), %rax
        callq __llvm_kcfi_dispatch_11111111
        callq __llvm_kcfi_dispatch_33333333
        retq

        .weak __kcfi_typeid_imported
__kcfi_typeid_imported = 0x11111111

        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        cmpl $0x11111111, -4(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_dispatch_33333333
        .globl __llvm_kcfi_dispatch_33333333
        .p2align 4
__llvm_kcfi_dispatch_33333333:
        cmpl $0x33333333, -4(%rax)
        jne __llvm_kcfi_mismatch_33333333
        jmpq *%rax

        .section .text,"xr",largest,__llvm_kcfi_mismatch_33333333
        .globl __llvm_kcfi_mismatch_33333333
__llvm_kcfi_mismatch_33333333:
        leaq __llvm_kcfi_list_33333333+8(%rip), %r10
        jmp __llvm_kcfi_open

        .section .rdata$llvm_kcfi_33333333_a,"dr"
        .p2align 3
        .globl __llvm_kcfi_list_33333333
__llvm_kcfi_list_33333333:
        .quad 0x33333333
        .section .rdata$llvm_kcfi_33333333_z,"dr"
        .p2align 3
        .quad 0x66666667

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
static_scanner:
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
