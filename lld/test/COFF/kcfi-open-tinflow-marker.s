# REQUIRES: x86

## Under -import-slots, a __kcfi_tinflow_ fact names its called type by the
## type's precise key followed by its check identifier. An object that opens a
## function type without a prototype publishes the precise key
## 1 << 32 | check, which stands for every precise type of that check
## identifier, so its opening reaches the fact of a called type with a
## precise key of its own and the same check identifier, in another object:
## here 0x22222222 opens without a prototype in marker.obj, and main.obj's
## call through precise type 0xabcdef01, of check identifier 0x22222222,
## hands back 0x33333333, which opens. The fact of a called type of another
## check identifier, precise type 0xabcdef02 of 0x55555555, handing back
## 0x44444444, does not follow.
## Another precise type of the same check identifier, 0xdeadbeef, which
## precise.obj opens, opens neither.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc marker.s -o marker.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc precise.s \
# RUN:   -o precise.obj
# RUN: lld-link main.obj marker.obj -import-slots -entry:main -debug:symtab \
# RUN:   -opt:ref -out:marker.exe
# RUN: llvm-objdump -d marker.exe | FileCheck %s --check-prefix=MARKER
# RUN: lld-link main.obj precise.obj -import-slots -entry:main -debug:symtab \
# RUN:   -opt:ref -out:precise.exe
# RUN: llvm-objdump -d precise.exe | FileCheck %s --check-prefix=PRECISE

# MARKER:      <__llvm_kcfi_dispatch_33333333>:
# MARKER:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_33333333>
# MARKER:      <__llvm_kcfi_dispatch_44444444>:
# MARKER:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# MARKER:      <__llvm_kcfi_mismatch_33333333>:
# MARKER-NEXT:   leaq {{.*}}(%rip), %r10
# MARKER-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>

# PRECISE:      <__llvm_kcfi_dispatch_33333333>:
# PRECISE:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# PRECISE:      <__llvm_kcfi_dispatch_44444444>:
# PRECISE:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# PRECISE-NOT:  <__llvm_kcfi_mismatch_

#--- marker.s
## An object that opens 0x22222222 without a prototype.
        .weak __kcfi_popen_0000000122222222
__kcfi_popen_0000000122222222 = 0x22222222

#--- precise.s
## An object that opens another precise type of check identifier 0x22222222.
        .weak __kcfi_popen_00000000deadbeef
__kcfi_popen_00000000deadbeef = 0x22222222

#--- main.s
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
main:
        callq __llvm_kcfi_dispatch_33333333
        callq __llvm_kcfi_dispatch_44444444
        retq

        .weak __kcfi_tinflow_0000000033333333_00000000abcdef0122222222
__kcfi_tinflow_0000000033333333_00000000abcdef0122222222 = 0x33333333
        .weak __kcfi_tinflow_0000000044444444_00000000abcdef0255555555
__kcfi_tinflow_0000000044444444_00000000abcdef0255555555 = 0x44444444

        .irpc d, 34
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
        movl $1, %eax

        .section .text,"xr",discard,__llvm_kcfi_open_dynamic
        .globl __llvm_kcfi_open_dynamic
        .p2align 4
__llvm_kcfi_open_dynamic:
        movl $2, %eax
