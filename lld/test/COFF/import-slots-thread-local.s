# REQUIRES: x86
## Under -import-slots, the loader copies an executable's thread-local template
## into the main thread's block before it binds the executable's imports, and
## initializes the DLLs before it calls the executable's TLS callbacks, so an
## in-place import slot in the template is bound in every later thread's block
## but not in the main thread's. The builtins' __llvm_thread_local_slot_copy,
## which the linker makes the executable's first TLS callback, copies each such
## slot from the template into the main thread's block, from a list that names,
## for each slot, its RVA and the lookup entry the template holds for it; the
## copy reads the image's own TLS directory for the index and template. The
## executable names that list as {tag, rva} at 0x1C in its MZ header for the
## fill of a DLL that may reach it first.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tl.s -o tl.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dir.s -o dir.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium res.s -o res.obj

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   tl.obj dir.obj res.obj lib.lib -out:tl.exe
# RUN: llvm-readobj --coff-tls-directory tl.exe > tl.txt
# RUN: llvm-objdump -s -j .CRT tl.exe >> tl.txt
# RUN: llvm-objdump -s -j .rdata tl.exe >> tl.txt
# RUN: od -A x -t x4 -j 0x1c -N 8 tl.exe >> tl.txt
# RUN: FileCheck --input-file=tl.txt %s

## The template holds its index and start RVAs; the slot is at offset 8 in it.
# CHECK:      StartAddressOfRawData: 0x140005000
# CHECK:      AddressOfIndex: 0x140003000

## The C initializer table holds no pointer to the fill, which the start-up
## calls directly; the TLS callback array, after its opening null, holds the
## copy's, which points into the image's code.
# CHECK:      Contents of section .CRT:
# CHECK-NEXT: 11111111 11111111 33333333 33333333
# CHECK-NEXT: 00000000 00000000 {{[0-9a-f]+}} 01000000
# CHECK-NEXT: 00000000 00000000

## The copy list: one slot, its count a 64-bit word; then the slot's RVA, the
## template start plus 8, and the lookup entry 0x20e8 the template holds for it.
# CHECK:      140002020 {{[0-9a-f]+ [0-9a-f]+}} 01000000 00000000
# CHECK-NEXT: 140002030 08500000 00000000 e8200000 00000000

## The MZ header names the copy list: the tag 0x9E3779B9 and its RVA.
# CHECK:      9e3779b9 00002028

## A DLL's template is bound before any thread's block is copied from it, an
## executable with no TLS directory gives no thread a block, and one with no
## slot in its template has nothing to copy: none carries a copy, and none names
## one in its header.
# RUN: lld-link -import-slots -dll -noentry crt.obj tl.obj dir.obj lib.lib \
# RUN:   -out:tl.dll
# RUN: od -A x -t x4 -j 0x1c -N 8 tl.dll | FileCheck --check-prefix=NOTAG %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   tl.obj lib.lib -out:nodir.exe
# RUN: od -A x -t x4 -j 0x1c -N 8 nodir.exe | FileCheck --check-prefix=NOTAG %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj crt.obj \
# RUN:   dir.obj res.obj lib.lib -out:noslot.exe
# RUN: od -A x -t x4 -j 0x1c -N 8 noslot.exe | FileCheck --check-prefix=NOTAG %s
# NOTAG: 00000000 00000000

## A DLL that imports from no executable has no fill, so nothing runs a copy:
## its start-up calls a function returning 0.
# RUN: llvm-objdump -d --no-show-raw-insn tl.dll | \
# RUN:   FileCheck --check-prefix=NOCALL %s
# NOCALL:      180001000: xorl %eax, %eax
# NOCALL-NEXT: retq
# NOCALL-NEXT: callq 0x180001000

## A DLL that imports from an executable may call it from its initializers
## before the executable's TLS callbacks run, so the linker sets bit 0 of its
## fill list's flag word; the fill runs the copy the executable's header names
## while the process initializes.
# RUN: lld-link -def:host.def -out:host.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium plug.s -o plug.obj
# RUN: lld-link -import-slots -dll -noentry crt.obj plug.obj host.lib \
# RUN:   -out:plug.dll
# RUN: llvm-objdump -d --no-show-raw-insn plug.dll > plug.txt
# RUN: llvm-objdump -s -j .rdata plug.dll >> plug.txt
# RUN: FileCheck --check-prefix=CALL --input-file=plug.txt %s

# CALL:      <.text>:
# CALL-NEXT: leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]
## No cell or residual word, one run-time cell; .sealed at 0x4000, the fill's
## done word and then the cell;
## the flag word's bit 0 set to run the executable's copy; the run-time cell for
## hostfn, bound in the executable (dll word 0), whose name follows the list.
# CALL:      [[#%x,LIST]]      00000000 00000000 00000000 01000000
# CALL-NEXT: [[#%x,LIST+0x10]] 00400000 10000000 {{[0-9a-f]+ [0-9a-f]+}}
# CALL-NEXT: [[#%x,LIST+0x20]] 01000000 00000000 08400000 {{[0-9a-f]+}}
# CALL-NEXT: [[#%x,LIST+0x30]] 00000000 {{.*}}

## A DLL with a run-time host import but no start-up that calls the fill cannot
## bind it.
# RUN: not lld-link -import-slots -dll -noentry plug.obj host.lib \
# RUN:   -out:nocrt.dll 2>&1 | FileCheck --check-prefix=NOCRTDLL %s
# NOCRTDLL: error: cannot bind __declspec(dllimport) hostfn, imported from host.exe, at run time: the image's start-up does not call __llvm_run_fill to write it

## Without a TLS callback array nothing runs the copy.
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   tl.obj dir.obj lib.lib -out:nocrt.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=NOCRT %s
# NOCRT: error: tl.obj: the address of an import in thread-local data needs a copy into the main thread's block, but the image has no TLS callback array (__xl_a) to run it

#--- lib.def
LIBRARY lib.dll
EXPORTS
  func
  arr DATA

#--- crt.s
## The C initializer table's bounds and the TLS callback array's, as the C
## runtime provides them, and stand-ins for the builtins' fill and copy, each
## loading its list.
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0x1111111111111111
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0x3333333333333333

## The image's start-up, which calls its fill directly.
  .section .text$start,"xr"
  .globl __start
__start:
  callq __llvm_run_fill
  retq
  .section .CRT$XLA,"dr"
  .globl __xl_a
__xl_a:
  .quad 0
  .section .CRT$XLZ,"dr"
  .globl __xl_z
__xl_z:
  .quad 0

  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  leaq __llvm_fill_list(%rip), %rax
  retq

  .def __llvm_thread_local_slot_copy
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_thread_local_slot_copy
  .globl __llvm_thread_local_slot_copy
__llvm_thread_local_slot_copy:
  leaq __llvm_thread_local_slot_copy_list(%rip), %rax
  retq

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
