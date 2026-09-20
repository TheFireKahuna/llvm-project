# REQUIRES: x86

# A .refptr.X stub holds the address of X for code that did not know where X
# lives. When X is in the image, the load of the stub becomes the address
# itself and the stub is dropped, as it is for an import pointer. A reference
# that cannot be rewritten keeps a pointer, and neither case is worth a
# warning, since the source never named an import.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/code.s -o %t.code.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/data.s -o %t.data.obj

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t.exe %t.code.obj 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
# RUN: llvm-objdump -d %t.exe | FileCheck --check-prefix=DISASM %s
# RUN: llvm-objdump -s %t.exe | FileCheck --check-prefix=NOPTR %s

# RUN: lld-link -entry:main -subsystem:console -out:%t.ptr.exe %t.code.obj %t.data.obj 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
# RUN: llvm-objdump -s %t.ptr.exe | FileCheck --check-prefix=PTR %s

# QUIET-NOT: locally defined symbol imported

# DISASM:      <main>:
# DISASM-NEXT:   48 8d 05 {{.*}} leaq {{.*}} <counter>
# DISASM-NEXT:   8b 00 movl
# DISASM-NEXT:   c3 retq

# Nothing is left in .rdata for the stub to occupy.
# NOPTR-NOT: Contents of section .rdata:

# The data reference reads the pointer, so one is emitted.
# PTR:      Contents of section .rdata:
# PTR-NEXT:  {{[0-9a-f]+}} 00300040 01000000

#--- code.s
        .global main
        .text
main:
        movq    .refptr.counter(%rip), %rax
        movl    (%rax), %eax
        ret

        .data
        .global counter
counter:
        .long   42

        .section .rdata$.refptr.counter,"dr",discard,.refptr.counter
        .global .refptr.counter
.refptr.counter:
        .quad   counter

#--- data.s
        .data
        .global holder
holder:
        .quad   .refptr.counter
