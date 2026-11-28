// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The entry point of an image whose fill has something to write, as
// x86_64/fill_entry.S describes.

#if defined(__aarch64__)

// A DLL's. A reason with bit 30 set, as 'FILL' has, goes to the exact
// compare, and any other reason with it set to the entry.
.macro fill_dll name, fill
  .section .text,"xr",one_only,\name
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 4
\name:
  tbnz w1, #30, 3f
  cmp w1, #1
  b.ne 2f
  .seh_proc \name\()_load
\name\()_load:
  stp x29, x30, [sp, #-32]!
  .seh_save_fplr_x 32
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  adrp x16, __llvm_fill_status
  ldr w16, [x16, :lo12:__llvm_fill_status]
  tbnz w16, #31, 5f
  stp x0, x2, [sp, #16]
  mov x1, x2
  mov w0, #0
  bl \fill
  tbnz w0, #31, 4f
  ldp x0, x2, [sp, #16]
  mov w1, #1
  adrp x16, __llvm_fill_attached
  strb w1, [x16, :lo12:__llvm_fill_attached]
  .seh_startepilogue
  ldp x29, x30, [sp], #32
  .seh_save_fplr_x 32
  .seh_endepilogue
1:
  b __llvm_fill_target
  .seh_endproc
2:
  b.hi 1b
  adrp x16, __llvm_fill_attached
  ldrb w16, [x16, :lo12:__llvm_fill_attached]
  cbnz w16, 1b
  mov w0, #1
  ret
3:
  mov w16, #0x4946
  movk w16, #0x4c4c, lsl #16
  cmp w1, w16
  b.ne 1b
  .seh_proc \name\()_again
\name\()_again:
  stp x29, x30, [sp, #-32]!
  .seh_save_fplr_x 32
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  mov w0, w1
  bl \fill
4:
  mov w16, w0
5:
  mvn w0, w16
  lsr w0, w0, #31
  .seh_startepilogue
  ldp x29, x30, [sp], #32
  .seh_save_fplr_x 32
  .seh_endepilogue
  ret
  .seh_endproc
.endm

// An executable's.
.macro fill_exe name, fill
  .section .text,"xr",one_only,\name
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 4
  .seh_proc \name
\name:
  stp x29, x30, [sp, #-32]!
  .seh_save_fplr_x 32
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  adrp x16, __llvm_fill_status
  ldr w0, [x16, :lo12:__llvm_fill_status]
  tbnz w0, #31, 1f
  str x0, [sp, #16]
  mov w0, #0
  bl \fill
  tbnz w0, #31, 1f
  ldr x0, [sp, #16]
  .seh_startepilogue
  ldp x29, x30, [sp], #32
  .seh_save_fplr_x 32
  .seh_endepilogue
  b __llvm_fill_target
1:
  adrp x16, __imp_RtlExitUserProcess
  ldr x16, [x16, :lo12:__imp_RtlExitUserProcess]
  .seh_startepilogue
  ldp x29, x30, [sp], #32
  .seh_save_fplr_x 32
  .seh_endepilogue
  br x16
  .seh_endproc
.endm

.macro fill_tls name, fill
  .section .text,"xr",one_only,\name
  .globl \name
  .def \name
  .scl 2
  .type 32
  .endef
  .p2align 2
\name:
  cmp w1, #1
  b.eq \name\()_attach
  ret
  .seh_proc \name\()_attach
\name\()_attach:
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  mov w0, #0
  bl \fill
  adrp x16, __llvm_fill_status
  str w0, [x16, :lo12:__llvm_fill_status]
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
  .seh_endproc
  .section .CRT$XLAF,"dr",associative,\name
  .p2align 3
  .xword \name
.endm

  fill_dll __llvm_fill_dll, __llvm_fill
  fill_dll __llvm_fill_dll_imports, __llvm_fill_imports
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
  mov w0, #1
  ret

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
