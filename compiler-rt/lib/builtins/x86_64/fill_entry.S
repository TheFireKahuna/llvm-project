// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The entry point of an image whose fill has something to write, which the
// linker makes the image's: it runs the fill, __llvm_fill or, in an image
// that binds imports by name, __llvm_fill_imports, or in a DLL whose
// thread-local template holds a word the fill writes their _threads form,
// and then
// __llvm_fill_target, which the linker binds to the entry that the image was
// linked with, or in a DLL with none to __llvm_fill_true. Each is its own
// section, so that an image holds only the one it starts with.

#if defined(__x86_64__)

// A DLL's. When the DLL is loaded it runs the fill first and fails the load
// where the fill fails; it answers a detach that follows such a failure
// itself, the entry not having seen the DLL attached, which a byte records;
// and for the reason 'FILL', which only NT-POSIX's exec gives, it runs the
// fill again, which a reason other than zero asks for. The load passes the
// fill the attach's reserved argument, which is null where the process loaded
// the DLL after it started, and 'FILL' passes the reason in its place. Code
// that moves no stack decides the reason and passes every other call on with
// a jump; each of the two calls of the fill is a function of its own.
.macro fill_dll name, fill
  .section .text,"xr",one_only,\name
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 4
\name:
  cmpl $1, %edx
  ja 2f
  jb 4f
  .seh_proc \name\()_load
  .seh_unwindversion 2
\name\()_load:
  subq $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  movl __llvm_fill_status(%rip), %eax
  testl %eax, %eax
  js 3f
  movq %rcx, 0x30(%rsp)
  movq %r8, 0x40(%rsp)
  movq %r8, %rdx
  xorl %ecx, %ecx
  callq \fill
  testl %eax, %eax
  js 3f
  movq 0x30(%rsp), %rcx
  movq 0x40(%rsp), %r8
  movl $1, %edx
  movb %dl, __llvm_fill_attached(%rip)
  .seh_startepilogue
  addq $0x28, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  jmp __llvm_fill_target
  .seh_endproc
2:
  cmpl $0x4C4C4946, %edx
  jne __llvm_fill_target
  .seh_proc \name\()_again
  .seh_unwindversion 2
\name\()_again:
  subq $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  movl %edx, %ecx
  callq \fill
3:
  notl %eax
  shrl $31, %eax
  .seh_startepilogue
  addq $0x28, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  retq
  .seh_endproc
4:
  cmpb %dl, __llvm_fill_attached(%rip)
  ja __llvm_fill_target
  movl $1, %eax
  retq
.endm

// An executable's: the fill, and then the entry, or ntdll's
// RtlExitUserProcess with the status of a fill that failed.
.macro fill_exe name, fill
  .section .text,"xr",one_only,\name
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 4
  .seh_proc \name
  .seh_unwindversion 2
\name:
  subq $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  movl __llvm_fill_status(%rip), %eax
  testl %eax, %eax
  js 1f
  movq %rcx, 0x30(%rsp)
  xorl %ecx, %ecx
  callq \fill
  testl %eax, %eax
  js 1f
  movq 0x30(%rsp), %rcx
  .seh_startepilogue
  addq $0x28, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  jmp __llvm_fill_target
1:
  movl %eax, %ecx
  .seh_startepilogue
  addq $0x28, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  jmpq *__imp_RtlExitUserProcess(%rip)
  .seh_endproc
.endm

// A TLS callback that runs the fill at the image's process attach. The linker
// makes it the first of an image whose array holds callbacks of its own, so
// that none of them runs before the fill; the entry then finds the fill done,
// or fails as it failed, which __llvm_fill_status holds. A callback's
// reserved argument is null whatever loaded the image, so it passes its
// reason, 1, in its place, which leaves that to the fill to find.
.macro fill_tls name, fill
  .section .text,"xr",one_only,\name
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 4
\name:
  cmpl $1, %edx
  je \name\()_attach
  retq
  .seh_proc \name\()_attach
  .seh_unwindversion 2
\name\()_attach:
  subq $0x28, %rsp
  .seh_stackalloc 0x28
  .seh_endprologue
  xorl %ecx, %ecx
  callq \fill
  movl %eax, __llvm_fill_status(%rip)
  .seh_startepilogue
  addq $0x28, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  retq
  .seh_endproc
  .section .CRT$XLAF,"dr",associative,\name
  .p2align 3
  .quad \name
.endm

  fill_dll __llvm_fill_dll, __llvm_fill
  fill_dll __llvm_fill_dll_imports, __llvm_fill_imports
  fill_dll __llvm_fill_dll_threads, __llvm_fill_threads
  fill_dll __llvm_fill_dll_imports_threads, __llvm_fill_imports_threads
  fill_exe __llvm_fill_exe, __llvm_fill
  fill_exe __llvm_fill_exe_imports, __llvm_fill_imports
  fill_tls __llvm_fill_tls, __llvm_fill
  fill_tls __llvm_fill_imports_tls, __llvm_fill_imports

// The entry of a DLL linked with none.
  .section .text,"xr",one_only,__llvm_fill_true
  .globl __llvm_fill_true
  .def __llvm_fill_true
  .scl 2
  .type 32
  .endef
__llvm_fill_true:
  movl $1, %eax
  retq

  .data
  .p2align 2
__llvm_fill_status:
  .long 0
__llvm_fill_attached:
  .byte 0

#ifdef __NTPOSIX__
// The builtins' import library of ntdll, which the fill imports from.
  .section .drectve,"yni"
  .ascii " -defaultlib:clang_rt.ntdll.lib"
#endif

#endif
