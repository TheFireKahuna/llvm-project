# REQUIRES: x86, aarch64

## An object that states the owned thread-local model in its _TLS_MODEL key
## has its .tls groups in the image's template, the read-only .tls section,
## whose header states its alignment; a foreign object's are the loader's
## template, which _tls_used describes. In an image with a template of its
## own the loader's is a read-only run of .rdata, between _tls_start and
## _tls_end, which the link defines where no foreign object does; its
## alignment is _tls_used's Characteristics. A SECREL counts from the start
## of the template that holds its target. An image where no object states the
## model keeps the loader's template in a writable .tls.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{t} = x86_64
# DEFINE: %{a} = x64
# DEFINE: %{own} = llvm-mc -filetype=obj -triple=%{t}-windows-ntposix
# DEFINE: %{for} = llvm-mc -filetype=obj -triple=%{t}-windows-msvc
# DEFINE: %{exe} = lld-link -machine:%{a} -entry:main -subsystem:console
# DEFINE: %{run} = \
# DEFINE:   %{own} --defsym %{a}=1 own.s -o own.obj && \
# DEFINE:   %{for} foreign.s -o foreign.obj && %{for} crt.s -o crt.obj && \
# DEFINE:   %{exe} own.obj foreign.obj crt.obj -out:mixed.exe && \
# DEFINE:   llvm-readobj --sections --coff-tls-directory mixed.exe > mixed.txt && \
# DEFINE:   llvm-objdump -d -s -j .text -j .data mixed.exe >> mixed.txt && \
# DEFINE:   FileCheck --check-prefixes=MIXED,MIXED-%{a} %s < mixed.txt && \
# DEFINE:   %{for} main.s -o main.obj && \
# DEFINE:   %{exe} main.obj foreign.obj crt.obj -out:foreign.exe && \
# DEFINE:   llvm-readobj --sections --coff-tls-directory foreign.exe > foreign.txt && \
# DEFINE:   llvm-objdump -s -j .data foreign.exe >> foreign.txt && \
# DEFINE:   FileCheck --check-prefix=FOREIGN %s < foreign.txt && \
# DEFINE:   %{own} --defsym %{a}=1 cross-own.s -o cross-own.obj && \
# DEFINE:   %{for} cross-foreign.s -o cross-foreign.obj && \
# DEFINE:   not %{exe} own.obj foreign.obj crt.obj cross-own.obj cross-foreign.obj \
# DEFINE:     -out:err.exe 2>&1 | FileCheck --check-prefix=CROSS %s && \
# DEFINE:   %{own} rec.s -o rec.obj && %{for} weak.s -o weak.obj && \
# DEFINE:   not %{exe} own.obj crt.obj rec.obj weak.obj -export:cw$tls,data \
# DEFINE:     -out:err.exe 2>&1 | FileCheck --check-prefix=RECORD %s && \
# DEFINE:   %{own} --defsym %{a}=1 index.s -o index.obj && \
# DEFINE:   not %{exe} own.obj foreign.obj crt.obj index.obj -out:err.exe \
# DEFINE:     2>&1 | FileCheck --check-prefix=INDEX %s && \
# DEFINE:   %{own} bound.s -o bound.obj && \
# DEFINE:   not %{exe} own.obj foreign.obj crt.obj bound.obj -out:err.exe \
# DEFINE:     2>&1 | FileCheck --check-prefix=BOUND %s && \
# DEFINE:   %{own} sel.s -o sel1.obj && %{own} sel.s -o sel2.obj && \
# DEFINE:   %{for} sel.s -o sel3.obj && \
# DEFINE:   not %{exe} own.obj crt.obj sel1.obj sel3.obj sel2.obj -out:err.exe \
# DEFINE:     2>&1 | FileCheck --check-prefix=SEL %s && \
# DEFINE:   not %{exe} own.obj crt.obj sel3.obj sel2.obj sel1.obj -out:err.exe \
# DEFINE:     2>&1 | FileCheck --check-prefix=SEL %s

# RUN: %{run}
# REDEFINE: %{t} = aarch64
# REDEFINE: %{a} = arm64
# RUN: %{run}

## The loader's template is fv and fw in .rdata, which the TLS directory
## bounds, with their alignment; the image's is tv and tw in .tls, with its
## own. A SECREL of each counts from its template's start.
# MIXED:       Name: .rdata
# MIXED-NEXT:  VirtualSize:
# MIXED-NEXT:  VirtualAddress: 0x[[#%X,RDATA:]]
# MIXED:       Name: .tls
# MIXED-NEXT:  VirtualSize: 0x8
# MIXED:       Characteristics [
# MIXED-NEXT:    IMAGE_SCN_ALIGN_16BYTES
# MIXED-NEXT:    IMAGE_SCN_CNT_INITIALIZED_DATA
# MIXED-NEXT:    IMAGE_SCN_MEM_READ
# MIXED-NEXT:  ]
# MIXED:       TLSDirectory {
# MIXED-NEXT:    StartAddressOfRawData: 0x[[#%X,0x140000000+RDATA]]
# MIXED-NEXT:    EndAddressOfRawData: 0x[[#%X,0x140000000+RDATA+0xC]]
# MIXED-NEXT:    AddressOfIndex:
# MIXED-NEXT:    AddressOfCallBacks: 0x0
# MIXED-NEXT:    SizeOfZeroFill: 0x0
# MIXED-NEXT:    Characteristics [ (0x500000)
# MIXED-NEXT:      IMAGE_SCN_ALIGN_16BYTES (0x500000)
# MIXED-NEXT:    ]
# MIXED-NEXT:  }
# MIXED:       Contents of section .data:
# MIXED-NEXT:  {{[0-9a-f]+}} 00000000 04000000
# MIXED-x64:   movl (%rax), %eax
# MIXED-x64-NEXT: movl 0x4(%rax), %eax
# MIXED-arm64: ldr w0, [x0]
# MIXED-arm64-NEXT: ldr w0, [x0, #0x4]

## With no object of the model the loader's template stays in a writable .tls.
# FOREIGN:     Name: .tls
# FOREIGN-NEXT: VirtualSize: 0xC
# FOREIGN-NEXT: VirtualAddress: 0x[[#%X,TLS:]]
# FOREIGN:       IMAGE_SCN_MEM_WRITE
# FOREIGN:     StartAddressOfRawData: 0x[[#%X,0x140000000+TLS]]
# FOREIGN-NEXT: EndAddressOfRawData: 0x[[#%X,0x140000000+TLS+0xC]]
# FOREIGN:     Contents of section .data:
# FOREIGN-NEXT: {{[0-9a-f]+}} 00000000 04000000

## Code of each model reaches the other's template, once for each variable;
## debug data describes either.
# CROSS:      error: thread-local model mismatch: fv
# CROSS-NEXT: >>> defined in the loader's template by foreign.obj
# CROSS-NEXT: >>> reached from the thread pointer by cross-own.obj
# CROSS-EMPTY:
# CROSS-NEXT: error: thread-local model mismatch: tv
# CROSS-NEXT: >>> defined in the image's template by own.obj
# CROSS-NEXT: >>> reached through _tls_index by cross-foreign.obj
# CROSS-NOT:  error:

## A record of the model whose variable the link took from a foreign object.
# RECORD:      error: thread-local model mismatch: cw
# RECORD-NEXT: >>> defined in the loader's template by weak.obj
# RECORD-NEXT: >>> reached through its record cw$tls by rec.obj

# INDEX:     error: index.obj: code of the ntposix thread-local model names _tls_index; its thread-locals are in the image's template
# INDEX-NOT: error:
# BOUND:     error: bound.obj: defines _tls_start, the bound of the loader's thread-local template, which the linker defines

## A selectany thread-local with copies of both models, in either order.
# SEL:      error: thread-local cv has copies of two models
# SEL-NEXT: >>> in the image's template: sel{{[12]}}.obj sel{{[12]}}.obj
# SEL-NEXT: >>> in the loader's template: sel3.obj

## wincrt's thread-end slot is kept in a Windows Itanium executable that
## imports the release and has a TLS callback of its own, here .CRT$XLB in an
## object or in bitcode, or refers to __cxa_thread_atexit_impl; never in a DLL,
## under -static, or for the callbacks of the slot's own archive.
# RUN: llvm-lib -machine:x64 -def:rt.def -out:rt.lib
# DEFINE: %{msvc} = llvm-mc -filetype=obj -triple=x86_64-windows-msvc
# RUN: %{msvc} tail.s -o tail.obj && %{msvc} xla.s -o xla.obj && \
# RUN:   %{msvc} cb.s -o cb.obj && \
# RUN:   llvm-mc -filetype=obj -triple=x86_64-windows-itanium atexit.s -o atexit.obj && \
# RUN:   %{msvc} static.s -o static.obj && \
# RUN:   llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o wi.obj && \
# RUN:   llvm-ar rcs wincrt.lib tail.obj xla.obj
# DEFINE: %{slot} = lld-link -machine:x64 -verbose wi.obj rt.lib wincrt.lib
# RUN: %{slot} -entry:main cb.obj -out:x.exe 2>&1 | FileCheck --check-prefix=SLOT %s
# RUN: opt cb.ll -o cb.bc
# RUN: %{slot} -entry:main cb.bc -out:x.exe 2>&1 | FileCheck --check-prefix=SLOT %s
# RUN: %{slot} -entry:main atexit.obj -include:f -out:x.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=SLOT %s
# RUN: %{slot} -entry:main -include:__xl_a -out:x.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=NOSLOT %s
# RUN: %{slot} -entry:main cb.obj static.obj -out:x.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=NOSLOT %s
# RUN: %{slot} -dll -noentry cb.obj -out:x.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=NOSLOT %s
# SLOT:       Loaded wincrt.lib(tail.obj) for __wincrt_thread_end_slot
# NOSLOT-NOT: __wincrt_thread_end_slot

## The TLS directory of an image of the model is wincrt's, whose member states
## the model: another library's member that offers _tls_used is never loaded,
## whichever library comes first, and the link defines the template's bounds
## it would have. An object's _tls_used in its place is an error.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dir.s -o dir.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium entry.s -o entry.obj
# RUN: %{msvc} tlssup.s -o tlssup.obj
# RUN: llvm-ar rcs dir.lib entry.obj dir.obj && llvm-ar rcs tlssup.lib tlssup.obj
# DEFINE: %{dir} = lld-link -machine:x64 -entry:main -verbose
# RUN: %{dir} tlssup.lib dir.lib -out:x.exe 2>&1 | FileCheck --check-prefix=DIR %s
# RUN: %{dir} dir.lib tlssup.lib -out:x.exe 2>&1 | FileCheck --check-prefix=DIR %s
# RUN: not %{dir} tlssup.obj dir.lib -out:x.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=DIR-OBJ %s
# DIR:     Loaded dir.lib(dir.obj) for _tls_used
# DIR-NOT: tlssup
# DIR-OBJ: error: tlssup.obj: defines _tls_used, which stands in place of the TLS directory of the image's thread-local model, in dir.lib(dir.obj)

#--- own.s
  .text
  .globl main
main:
.ifdef x64
  movl tv@SECREL32(%rax), %eax
  movl tw@SECREL32(%rax), %eax
.else
  ldr w0, [x0, :secrel_lo12:tv]
  ldr w0, [x0, :secrel_lo12:tw]
.endif
  ret
  .section .tls$,"dw"
  .p2align 4
  .globl tv
tv:
  .long 1
  .section .tls$b,"dw"
  .globl tw
tw:
  .long 2

#--- main.s
  .text
  .globl main
main:
  ret

#--- foreign.s
  .data
  .secrel32 fv
  .secrel32 fw
  .section .tls$,"dw"
  .p2align 4
  .globl fv
fv:
  .long 3
  .section .tls$zzz,"dw"
  .globl fw
fw:
  .quad 4

#--- crt.s
  .section .rdata$T,"dr"
  .globl _tls_used
_tls_used:
  .quad _tls_start, _tls_end, _tls_index, 0
  .long 0, 0
  .data
  .globl _tls_index
_tls_index:
  .long 0
  .section .drectve,"yn"
  .ascii " /include:_tls_used"

#--- cross-own.s
  .text
.ifdef x64
  movl fv@SECREL32(%rax), %eax
  movl fv@SECREL32(%rax), %eax
.else
  ldr w0, [x0, :secrel_lo12:fv]
.endif
  .section .debug_info,"dr"
  .secrel32 fw

#--- cross-foreign.s
  .data
  .secrel32 tv

#--- rec.s
  .section .tls$,"dw"
  .weak cw
cw:
  .long 1
  .section .tls$,"dw",discard,__tls_set
  .globl __tls_set
__tls_set:
  .byte 1
  .link_fact cw, 7, __tls_set

#--- weak.s
  .section .tls$,"dw"
  .globl cw
cw:
  .long 2

#--- index.s
  .text
.ifdef x64
  movl _tls_index(%rip), %eax
.else
  adrp x0, _tls_index
.endif
  .data
  .quad _tls_index

#--- bound.s
  .data
  .globl _tls_start
_tls_start:
  .quad 0

#--- sel.s
  .section .tls$,"dw",discard,cv
  .globl cv
cv:
  .long 5

#--- dir.s
  .section .rdata$T,"dr"
  .globl _tls_used
_tls_used:
  .quad _tls_start, _tls_end, _tls_index, 0
  .long 0, 0
  .data
  .globl _tls_index
_tls_index:
  .long 0

#--- entry.s
  .text
  .globl main
main:
  ret
  .section .drectve,"yn"
  .ascii " /include:_tls_used"

#--- tlssup.s
  .section .rdata$T,"dr"
  .globl _tls_used
_tls_used:
  .quad _tls_start, _tls_end, _tls_index, 0
  .long 0, 0
  .data
  .globl _tls_index
_tls_index:
  .long 0
  .section .tls,"dw"
  .globl _tls_start
_tls_start:
  .byte 0
  .section .tls$ZZZ,"dw"
  .globl _tls_end
_tls_end:
  .byte 0

#--- rt.def
LIBRARY rt.dll
EXPORTS
__wincrt_thread_end
__cxa_thread_atexit_impl

#--- tail.s
## The slot is a callback of the slot object's own, which calls the release
## through its import in the runtime; here it returns.
  .section .CRT$XLY,"dr"
  .globl __wincrt_thread_end_slot
__wincrt_thread_end_slot:
  .quad __wincrt_thread_tail
  .text
  .globl __wincrt_thread_tail
__wincrt_thread_tail:
  ret
  .section .drectve,"yn"
  .ascii " -export:__wincrt_thread_tail"

#--- xla.s
  .section .CRT$XLA,"dr"
  .globl __xl_a
__xl_a:
  .quad 0

#--- cb.s
  .section .CRT$XLB,"dr"
  .quad main

#--- atexit.s
  .text
  .globl f
f:
  call __cxa_thread_atexit_impl

#--- static.s
  .text
  .globl __wincrt_thread_end
__wincrt_thread_end:
  ret

#--- cb.ll
target triple = "x86_64-pc-windows-itanium"

@cb = global ptr @f, section ".CRT$XLB"

define void @f() {
  ret void
}
