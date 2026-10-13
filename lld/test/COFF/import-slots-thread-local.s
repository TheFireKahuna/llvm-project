# REQUIRES: x86
## Under -import-slots, the loader copies an executable's thread-local template
## into the main thread's block before it binds the executable's imports, and
## initializes the DLLs before it calls the executable's TLS callbacks, so an
## in-place import slot in the template is bound in every later thread's block
## but not in the main thread's. The executable's first TLS callback copies each
## such slot from the template into the main thread's block, found through the
## index and the template start that the TLS directory names. It acts only
## while PEB->CrossProcessFlags says the process is initializing, and only on a
## word that still holds its lookup entry, so it may run again. The executable
## names it as {tag, rva} at 0x1C in its MZ header for code that may reach the
## executable while the DLLs initialize.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tl.s -o tl.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dir.s -o dir.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium res.s -o res.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium bad.s -o bad.obj

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   tl.obj dir.obj res.obj lib.lib -out:tl.exe
# RUN: llvm-readobj --coff-tls-directory tl.exe > tl.txt
# RUN: llvm-objdump -d --print-imm-hex tl.exe >> tl.txt
# RUN: llvm-objdump -s -j .CRT tl.exe >> tl.txt
# RUN: od -A x -t x1 -j 0x1c -N 8 tl.exe >> tl.txt
# RUN: FileCheck --input-file=tl.txt %s

## The slot is at offset 8 in the template, after another thread-local datum;
## the template holds its lookup entry, 0x2098.
# CHECK:      StartAddressOfRawData: 0x[[#%x,START:]]
# CHECK:      AddressOfIndex: 0x[[#%x,INDEX:]]
# CHECK:      140001010: {{.*}} movq %gs:0x60, %rax
# CHECK-NEXT: btl $0x1, 0x50(%rax)
# CHECK-NEXT: jae 0x[[#%x,DONE:]]
# CHECK-NEXT: movl {{.*}}(%rip), %ecx # 0x[[#INDEX]]
# CHECK-NEXT: movq %gs:0x58, %rdx
# CHECK-NEXT: movq (%rdx,%rcx,8), %rdx
# CHECK-NEXT: movq 0x8(%rdx), %rax
# CHECK-NEXT: movabsq $0x2098, %r8
# CHECK-NEXT: cmpq %r8, %rax
# CHECK-NEXT: jne 0x[[#DONE]]
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#START + 8]]
# CHECK-NEXT: movq %rax, 0x8(%rdx)
# CHECK-NEXT: [[#DONE]]: {{.*}} xorl %eax, %eax
# CHECK-NEXT: retq

## The copy's pointer follows the TLS callback array's opening null; the C
## initializer table holds only the residual fill's.
# CHECK:      Contents of section .CRT:
# CHECK-NEXT: 11111111 11111111 60100040 01000000
# CHECK-NEXT: 33333333 33333333 00000000 00000000
# CHECK-NEXT: 10100040 01000000 00000000 00000000

## The MZ header names the copy: the tag 0x9E3779B9 and its RVA.
# CHECK:      b9 79 37 9e 10 10 00 00

## A DLL's template is bound before any thread's block is copied from it, an
## executable with no TLS directory gives no thread a block, and one with no
## slot in its template has nothing to copy: none carries a copy, and none
## names one.
# RUN: lld-link -import-slots -dll -noentry crt.obj tl.obj dir.obj lib.lib \
# RUN:   -out:tl.dll
# RUN: llvm-objdump -d tl.dll | FileCheck --check-prefix=NONE %s
# RUN: od -A x -t x1 -j 0x1c -N 8 tl.dll | FileCheck --check-prefix=NOTAG %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   tl.obj lib.lib -out:nodir.exe
# RUN: llvm-objdump -d nodir.exe | FileCheck --check-prefix=NONE %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   dir.obj res.obj lib.lib -out:noslot.exe
# RUN: llvm-objdump -d noslot.exe | FileCheck --check-prefix=NONE %s
# RUN: od -A x -t x1 -j 0x1c -N 8 noslot.exe | FileCheck --check-prefix=NOTAG %s
# NONE-NOT: %gs:0x58
# NOTAG: 00 00 00 00 00 00 00 00

## A DLL that imports from an executable may call it from its initializers
## before the executable's TLS callbacks run, so its C initializer table runs
## the copy the executable names first, ahead of every other initializer, while
## the process initializes; through the guard's dispatch where the image has
## it, here the first word of .rdata, or directly where it has none.
# RUN: lld-link -def:host.def -out:host.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plug.s -o plug.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium guard.s -o guard.obj
# RUN: lld-link -import-slots -dll -noentry crt.obj plug.obj guard.obj \
# RUN:   host.lib -out:plug.dll
# RUN: llvm-readobj --sections plug.dll > plug.txt
# RUN: llvm-objdump -d --print-imm-hex plug.dll >> plug.txt
# RUN: llvm-objdump -s -j .CRT plug.dll >> plug.txt
# RUN: FileCheck --check-prefix=CALL --input-file=plug.txt %s
# CALL:      Name: .rdata
# CALL-NEXT: VirtualSize:
# CALL-NEXT: VirtualAddress: 0x[[#%x,RDATA:]]
# CALL:      180001010: {{.*}} movq %gs:0x60, %rax
# CALL-NEXT: btl $0x1, 0x50(%rax)
# CALL-NEXT: jae 0x[[#%x,ZERO:]]
# CALL-NEXT: movq 0x10(%rax), %rax
# CALL-NEXT: cmpl $0x9e3779b9, 0x1c(%rax)
# CALL-NEXT: jne 0x[[#ZERO]]
# CALL-NEXT: movl 0x20(%rax), %ecx
# CALL-NEXT: addq %rcx, %rax
# CALL-NEXT: jmpq *{{.*}}(%rip) # 0x[[#RDATA + 0x180000000]]
# CALL-NEXT: [[#ZERO]]: {{.*}} xorl %eax, %eax
# CALL-NEXT: retq
# CALL:      Contents of section .CRT:
# CALL-NEXT: 11111111 11111111 10100080 01000000

# RUN: lld-link -import-slots -dll -noentry crt.obj plug.obj host.lib \
# RUN:   -out:noguard.dll
# RUN: llvm-objdump -d noguard.dll | FileCheck --check-prefix=NOGUARD %s
# NOGUARD:      addq %rcx, %rax
# NOGUARD-NEXT: jmpq *%rax

## A DLL that imports from no executable, or that has no C initializer table,
## carries none.
# RUN: llvm-objdump -d tl.dll | FileCheck --check-prefix=NOCALL %s
# RUN: lld-link -import-slots -dll -noentry plug.obj host.lib -out:nocrt.dll
# RUN: llvm-objdump -d nocrt.dll | FileCheck --check-prefix=NOCALL %s
# NOCALL-NOT: 0x9e3779b9

## Without a TLS callback array nothing runs the copy, and a TLS directory that
## names no template or index leaves no block to copy into.
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   tl.obj dir.obj lib.lib -out:nocrt.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=NOCRT %s
# NOCRT: error: tl.obj: the address of an import in thread-local data needs a copy into the main thread's block, but the image has no TLS callback array (__xl_a) to run it
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   crt.obj tl.obj bad.obj lib.lib -out:bad.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD %s
# BAD: error: tl.obj: the address of an import in thread-local data needs a copy into the main thread's block, but the image's TLS directory (_tls_used) names no template or index

#--- lib.def
LIBRARY lib.dll
EXPORTS
  func
  arr DATA

#--- crt.s
## The C initializer table's bounds and the TLS callback array's, as the C
## runtime provides them.
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0x1111111111111111
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0x3333333333333333
  .section .CRT$XLA,"dr"
  .globl __xl_a
__xl_a:
  .quad 0
  .section .CRT$XLZ,"dr"
  .globl __xl_z
__xl_z:
  .quad 0

#--- main.s
  .text
  .globl main
main:
  retq

#--- tl.s
  .section .tls$,"dw"
  .p2align 3
  .quad func

#--- dir.s
  .section .tls,"dw"
  .globl _tls_start
_tls_start:
  .quad 0
  .section .tls$ZZZ,"dw"
  .globl _tls_end
_tls_end:
  .quad 0
  .data
  .globl _tls_index
_tls_index:
  .long 0
  .section .rdata,"dr"
  .globl _tls_used
  .p2align 3
_tls_used:
  .quad _tls_start
  .quad _tls_end
  .quad _tls_index
  .quad 0
  .long 0
  .long 0

#--- res.s
## A word inside an import for which its DLL exports no name, which the
## residual fill writes.
  .data
  .p2align 3
  .quad arr+8

#--- host.def
LIBRARY host.exe
EXPORTS
  hostfn

#--- plug.s
  .text
  .globl dllmain
dllmain:
  jmp hostfn

#--- guard.s
## The guard's dispatch pointer, as the load configuration provides it.
  .section .rdata,"dr"
  .globl __guard_dispatch_icall_fptr
  .p2align 3
__guard_dispatch_icall_fptr:
  .quad 0

#--- bad.s
  .section .rdata,"dr"
  .globl _tls_used
  .p2align 3
_tls_used:
  .quad 0
  .quad 0
  .quad 0
  .quad 0
  .long 0
  .long 0
