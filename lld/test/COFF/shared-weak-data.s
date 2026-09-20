# REQUIRES: x86

# A COMDAT variable the compiler left preemptable is reached through a
# .refptr stub. When a DLL in the link offers a copy of it, this image binds
# to that one and drops its own, together with the initializer the compiler
# put in the same COMDAT, and forwards its export of the name to the DLL. A
# reference that reaches the variable directly cannot be redirected, so it
# keeps the local copy.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:shared,DATA -implib:%t.lib.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/direct.s -o %t.direct.obj

# RUN: lld-link -entry:main -subsystem:console -out:%t.exe %t.main.obj %t.lib.lib
# RUN: llvm-readobj --coff-imports --coff-exports %t.exe | FileCheck --check-prefix=SHARED %s
# RUN: llvm-objdump -s -j .data %t.exe | FileCheck --check-prefix=NOCOPY %s

## A direct reference to the variable keeps this image's copy.
# RUN: lld-link -entry:main -subsystem:console -out:%t.direct.exe %t.main.obj %t.direct.obj %t.lib.lib
# RUN: llvm-readobj --coff-imports %t.direct.exe | FileCheck --check-prefix=LOCAL %s
# RUN: llvm-objdump -s -j .data %t.direct.exe | FileCheck --check-prefix=COPY %s

# SHARED:      Symbol: shared
# SHARED:      Name: shared
# SHARED-NEXT: ForwardedTo: shared-weak-data.s.tmp.lib.shared

# The local copy and the pointer to its initializer are both gone.
# NOCOPY-NOT: Contents of section .data:

# LOCAL-NOT: Symbol: shared
# COPY:      Contents of section .data:
# COPY-NEXT:  {{[0-9a-f]+}} 2a000000

#--- lib.s
        .global shared
        .data
shared:
        .long   99

#--- main.s
        .section .drectve,"yn"
        .ascii " /EXPORT:shared,DATA"

        .global main
        .text
main:
        movq    .refptr.shared(%rip), %rax
        movl    (%rax), %eax
        ret

# The variable, its initializer pointer in an associated COMDAT, and the stub,
# as the compiler emits them for a preemptable COMDAT definition.
        .section .data,"dw",discard,shared
        .global shared
shared:
        .long   42

        .section .CRT$XCU,"dw",associative,shared
        .quad   init

        .text
init:
        ret

        .section .rdata$.refptr.shared,"dr",discard,.refptr.shared
        .global .refptr.shared
.refptr.shared:
        .quad   shared

#--- direct.s
        .global reader
        .text
reader:
        movl    shared(%rip), %eax
        ret
