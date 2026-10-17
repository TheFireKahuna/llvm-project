# REQUIRES: x86

## Only the facts of objects in the link open KCFI types. An archive member
## that publishes that it opens 0x11111111 dynamically, through which a
## pointer of 0x22222222 can come back, opens 0x22222222 when it is loaded,
## and leaves it closed when nothing loads it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc use.s -o use.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc opener.s \
# RUN:   -o opener.obj
# RUN: llvm-lib -out:opener.lib opener.obj

# RUN: lld-link main.obj opener.lib -entry:main -debug:symtab -out:closed.exe
# RUN: llvm-objdump -d closed.exe | FileCheck %s --check-prefix=CLOSED
# RUN: lld-link main.obj use.obj opener.lib -entry:main -debug:symtab \
# RUN:   -out:open.exe
# RUN: llvm-objdump -d open.exe | FileCheck %s --check-prefix=OPEN

# CLOSED:     <__llvm_kcfi_dispatch_22222222>:
# CLOSED:       jne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# CLOSED-NOT: <__llvm_kcfi_mismatch_

# OPEN:      <__llvm_kcfi_dispatch_22222222>:
# OPEN:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_22222222>
# OPEN:      <__llvm_kcfi_mismatch_22222222>:
# OPEN-NEXT:   leaq {{.*}}(%rip), %r10
# OPEN-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>

#--- opener.s
  .linktypeprefixes
        .weak __kcfi_popen_0000000011111111
__kcfi_popen_0000000011111111 = 0x11111111
        .text
        .globl opener
opener:
        retq

#--- use.s
  .linktypeprefixes
        .text
        .globl use
use:
        callq opener
        retq

#--- main.s
  .linktypeprefixes
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
main:
        callq __llvm_kcfi_dispatch_22222222
        retq

        .weak __kcfi_tinflow_0000000022222222_0000000011111111
__kcfi_tinflow_0000000022222222_0000000011111111 = 0x22222222

        .weak __llvm_kcfi_mismatch_22222222
__llvm_kcfi_mismatch_22222222 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_dispatch_22222222
        .globl __llvm_kcfi_dispatch_22222222
        .p2align 4
__llvm_kcfi_dispatch_22222222:
        cmpl $0x22222222, -4(%rax)
        jne __llvm_kcfi_mismatch_22222222
        jmpq *%rax

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
