# REQUIRES: x86, aarch64

## The CodeView a PDB receives has the image's code offsets and lengths where
## the link resized code: line tables, procedures, blocks, thunks, heap sites,
## ranges and inline-site annotations; forms it does not remap are reported.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium warn.s -o warn.obj
# DEFINE: %{link} = lld-link -dll -noentry -export:f -debug -debugtype:cv,fixup
# DEFINE: %{dump} = llvm-pdbutil dump -l -symbols -fixups

# RUN: %{link} -pdb:x64.pdb x64.obj -out:x64.dll 2>&1 | count 0
# RUN: llvm-objdump -d --no-show-raw-insn x64.dll | FileCheck --check-prefix=X64-DIS %s
# RUN: %{dump} x64.pdb | FileCheck --check-prefixes=LINES,X64-LINES,SYMS,X64,X64-FIXUP %s
# RUN: %{link} -machine:arm64 -pdb:a64.pdb a64.obj -out:a64.dll 2>&1 | count 0
# RUN: llvm-objdump -d --no-show-raw-insn a64.dll | FileCheck --check-prefix=A64-DIS %s
# RUN: %{dump} a64.pdb | FileCheck --check-prefixes=LINES,A64-LINES,SYMS,A64 %s
# RUN: %{link} -pdb:warn.pdb warn.obj -out:warn.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s

## f: the je and the call are short, the read and the tail jump are gone.
# X64-DIS:      <f>:
# X64-DIS-NEXT: 180001000: je 0x180001011
# X64-DIS-NEXT: 180001002: callq 0x180001011
# X64-DIS-NEXT: 180001007: nopw
# X64-DIS-NEXT: 180001010: nop
# X64-DIS-NEXT: 180001011: retq
## j.
# X64-DIS:      180010077: je 0x18001007a
# X64-DIS-NEXT: 180010079: nop
# X64-DIS-NEXT: 18001007a: retq

# A64-DIS:      <f>:
# A64-DIS-NEXT: 180001000: adr x0
# A64-DIS-NEXT: 180001004: bl 0x180001014
# A64-DIS-NEXT: 180001008: nop
# A64-DIS-NEXT: 18000100c: nop
# A64-DIS-NEXT: 180001010: nop
# A64-DIS-NEXT: 180001014: ret

## f: line 2 is the deleted read's or add's, line 4 that of the padding, which
## line 5 follows, and line 6 the deleted jump's.
# LINES:           Lines
# X64-LINES:       0001:00000000-00000011, line/addr entries = 3
# X64-LINES-NEXT:  1 00000000 ! 3 00000002 ! 5 00000010 !
# A64-LINES:       0001:00000000-00000014, line/addr entries = 3
# A64-LINES-NEXT:  1 00000000 ! 3 00000004 ! 5 00000010 !
## i.
# X64-LINES:       0001:00000020-00000052, line/addr entries = 2
# X64-LINES-NEXT:  20 00000020 ! 21 00000051 !
# A64-LINES:       0001:00000020-00000048, line/addr entries = 2
# A64-LINES-NEXT:  20 00000020 ! 21 00000044 !
## c and its separated code.
# X64-LINES:       0001:0000F07B-0000F081, line/addr entries = 2
# X64-LINES-NEXT:  50 0000F07B ! 51 0000F080 !
# X64-LINES:       0001:0000F081-0000F085, line/addr entries = 2
# X64-LINES-NEXT:  50 0000F081 ! 52 0000F084 !
# A64-LINES:       0001:0000F05C-0000F06C, line/addr entries = 2
# A64-LINES-NEXT:  50 0000F05C ! 51 0000F068 !
# A64-LINES:       0001:0000F06C-0000F078, line/addr entries = 2
# A64-LINES-NEXT:  50 0000F06C ! 52 0000F074 !
## j: line 41 is the jmp's.
# X64-LINES:       0001:0000F077-0000F07B, line/addr entries = 3
# X64-LINES-NEXT:  40 0000F077 ! 42 0000F079 ! 43 0000F07A !

# SYMS:      Symbols
# SYMS:      S_GPROC32 [size = 44] `f`
# X64-NEXT:  addr = 0001:0000, code size = 17
# X64-NEXT:  debug start = 2, debug end = 17
# A64-NEXT:  addr = 0001:0000, code size = 20
# A64-NEXT:  debug start = 4, debug end = 20
## The first range is the deleted instruction's and holds nothing. The gap is
## the call and the padding, at which the label that ends it stands.
# SYMS:      S_DEFRANGE_REGISTER [size = 20]
# X64-NEXT:  range start = 0001:0002, length = 15
# X64-NEXT:  gaps = [(0,14)]
# A64-NEXT:  range start = 0001:0004, length = 16
# A64-NEXT:  gaps = [(0,12)]
# SYMS-NEXT: S_BLOCK32
# SYMS-NEXT: parent = 4, end = 104
# X64-NEXT:  code size = 15, addr = 0001:0002
# A64-NEXT:  code size = 16, addr = 0001:0004
# SYMS:      S_HEAPALLOCSITE
# X64-NEXT:  addr = 0001:0002 call size = 5
# A64-NEXT:  addr = 0001:0004 call size = 4

# SYMS:      S_GPROC32 [size = 44] `i`
# X64-NEXT:  addr = 0001:0032, code size = 50
# A64-NEXT:  addr = 0001:0032, code size = 40
## On x86-64 the record is four bytes shorter than in the object.
# SYMS:      S_INLINESITE [size = 28]
# SYMS-NEXT: inlinee = 0x1000 (inl), parent = 128, end = 200
# X64-NEXT:  0B01 code 0x1 (+0x1) line 0 (-0)
# X64-NEXT:  0B2D code 0xE (+0xD) line 1 (+1)
# X64-NEXT:  0B2D code 0x1B (+0xD) line 2 (+1)
# X64-NEXT:  0602 line 3 (+1)
# X64-NEXT:  0315 code 0x30 (+0x15)
# X64-NEXT:  0401 code end 0x31 (+0x1)
# A64-NEXT:  0B04 code 0x4 (+0x4) line 0 (-0)
# A64-NEXT:  0B2C code 0x10 (+0xC) line 1 (+1)
# A64-NEXT:  0B24 code 0x14 (+0x4) line 2 (+1)
# A64-NEXT:  0B2C code 0x20 (+0xC) line 3 (+1)
# A64-NEXT:  0404 code end 0x24 (+0x4)
# SYMS:      S_INLINESITE_END

## big: the range was 61440 bytes long, and on x86-64 a record of that length
## and one of 16 bytes, and is four bytes longer.
# SYMS:      S_GPROC32 [size = 44] `big`
# X64-NEXT:  addr = 0001:0096, code size = 61463
# A64-NEXT:  addr = 0001:0080, code size = 61452
# SYMS:      S_DEFRANGE_REGISTER [size = 16]
# X64-NEXT:  range start = 0001:0098, length = 61440
# A64-NEXT:  range start = 0001:0084, length = 61440
# SYMS-NEXT: gaps = []
# SYMS-NEXT: S_DEFRANGE_REGISTER [size = 16]
# X64-NEXT:  range start = 0001:61538, length = 4
# A64-NEXT:  range start = 0001:61524, length = 4
# SYMS-NEXT: gaps = []
# X64-NEXT:  S_DEFRANGE_REGISTER [size = 16]
# X64-NEXT:  range start = 0001:61542, length = 16
# X64-NEXT:  gaps = []
# SYMS-NEXT: S_END

## c: inline-site offsets count from the procedure, then from its separated
## code after a change of base; the record after S_END names and sizes it.
# SYMS:      S_GPROC32 [size = 44] `c`
# X64-NEXT:  addr = 0001:61563, code size = 6
# A64-NEXT:  addr = 0001:61532, code size = 16
# SYMS:      S_INLINESITE [size = 32]
# X64:       0B02 code 0x2 (+0x2) line 0 (-0)
# X64-NEXT:  0B21 code 0x3 (+0x1) line 1 (+1)
# X64-NEXT:  0402 code end 0x5 (+0x2)
# X64-NEXT:  0201 code base 1
# X64-NEXT:  0B20 code 0x0 (+0x0) line 2 (+1)
# X64-NEXT:  0B22 code 0x2 (+0x2) line 3 (+1)
# X64-NEXT:  0401 code end 0x3 (+0x1)
# A64:       0B04 code 0x4 (+0x4) line 0 (-0)
# A64-NEXT:  0B24 code 0x8 (+0x4) line 1 (+1)
# A64-NEXT:  0404 code end 0xC (+0x4)
# A64-NEXT:  0201 code base 1
# A64-NEXT:  0B20 code 0x0 (+0x0) line 2 (+1)
# A64-NEXT:  0B24 code 0x4 (+0x4) line 3 (+1)
# A64-NEXT:  0404 code end 0x8 (+0x4)
# SYMS:      S_SEPCODE [size = 32]
# SYMS-NEXT: parent = 0, end =
# X64-NEXT:  code size = 4, addr = 0001:61569, parent addr = 0001:61563
# A64-NEXT:  code size = 12, addr = 0001:61548, parent addr = 0001:61532
# SYMS-NEXT: flags = none
# SYMS-NEXT: S_END

# X64:       S_THUNK32 [size = 28] `k`
# X64-NEXT:  parent = 0, end = 464, next = 0
# X64-NEXT:  kind = thunk, size = 3, addr = 0001:0018

## The Fixup stream has each jump where the symbols have its function: at
## f + 1, i + 2, i + 15, big + 1 and j + 1.
# X64-FIXUP:      8022 | 0000 | 00001001 | 00001011
# X64-FIXUP:      8022 | 0000 | 00001022 | 00001011
# X64-FIXUP-NEXT: 8022 | 0000 | 0000102F | 00001011
# X64-FIXUP-NEXT: 8022 | 0000 | 00001061 | 00001011
# X64-FIXUP-NEXT: 8022 | 4000 | 00010078 | 0001007A

# WARN:      warning: warn.obj: a symbol record of kind 0x112C is not remapped to the code that the link resized
# WARN-NEXT: warning: warn.obj: an S_INLINESITE2 record is not remapped to the code that the link resized
# WARN-NEXT: warning: warn.obj: the line table of an inline site is not remapped to the code that the link resized
# WARN-NEXT: warning: warn.obj: frame data is not remapped to the code that the link resized
#--- types.inc
.macro proc name, end, dbgstart=0, dbgend=0
  .short .L\name\()p1-.L\name\()p0
.L\name\()p0:
  .short 4368
  .long 0, 0, 0
  .long \end-\name
  .long \dbgstart, \dbgend, 0
  .secrel32 \name
  .secidx \name
  .byte 0
  .asciz "\name"
  .p2align 2, 0
.L\name\()p1:
.endm
.macro fn name, sect
  .section \sect,"xr",discard,\name
  .globl \name
\name:
.endm
  .section .debug$T,"dr"
  .long 4
  .short 0xa, 0x1201
  .long 1, 0x74
  .short 0xe, 0x1008
  .long 0x74
  .byte 0, 0
  .short 1
  .long 0x1000
  .short 0xe, 0x1601
  .long 0, 0x1001
  .asciz "inl"

#--- cv.inc
  .section .debug$S,"dr"
  .long 4
  .long 241
  .long .Lsyms_end-.Lsyms
.Lsyms:
  proc f, .Lf_end, .Lf1-f, .Lf4-f
  .short .L3-.L2
.L2:
  .short 4414
  .long 116
  .short 0
  .asciz "v"
  .p2align 2, 0
.L3:
  .cv_def_range .Lf1 .Lf2 .Lf3 .Lf4, reg, 17
  .short .L5-.L4
.L4:
  .short 4355
  .long 0, 0
  .long .Lf4-.Lf1
  .secrel32 .Lf1
  .secidx .Lf1
  .byte 0
  .p2align 2, 0
.L5:
  .short 2, 6
  .short .L7-.L6
.L6:
  .short 4446
  .secrel32 .Lf2
  .secidx .Lf2
  .short .Lf3-.Lf2
  .long 116
  .p2align 2, 0
.L7:
  .short 2, 6
  proc i, .Li_end
  .short .L13-.L12
.L12:
  .short 4429
  .long 0, 0
  .long 0x1002
  .cv_inline_linetable 3 1 30 i .Li_end
  .p2align 2, 0
.L13:
  .short 2, 4430, 2, 6
  proc big, .Lbig_end
  .short .L17-.L16
.L16:
  .short 4414
  .long 116
  .short 0
  .asciz "w"
  .p2align 2, 0
.L17:
  .cv_def_range .Lbig1 .Lbig2, reg, 18
  .short 2, 6
  proc c, .Lc_end
  .short .L23-.L22
.L22:
  .short 4429
  .long 0, 0
  .long 0x1002
  .cv_inline_linetable 6 1 60 c .Lc_end
  .p2align 2, 0
.L23:
  .short 2, 4430, 2, 6
  .short .L25-.L24
.L24:
  .short 4402
  .long 0, 0, .Lc_cold_end-c.cold, 0
  .secrel32 c.cold
  .secrel32 c
  .secidx c.cold
  .secidx c
.L25:
  .short 2, 6
.Lsyms_end:
  .p2align 2, 0
  .cv_linetable 0, f, .Lf_end
  .cv_linetable 2, i, .Li_end
  .cv_linetable 5, c, .Lc_end
  .cv_linetable 5, c.cold, .Lc_cold_end

#--- x64.s
  .include "types.inc"
  .cv_file 1 "t.c"

## Lines, a procedure, a block, a heap allocation site and ranges.
  fn f, .text$a
  .cv_func_id 0
  .cv_loc 0 1 1 0
  je g
.Lf1:
  .cv_loc 0 1 2 0
  movq __imp_g@FUSED(%rip), %rax
  .cv_loc 0 1 3 0
.Lf2:
  callq *__imp_g(%rip)
.Lf3:
  .cv_loc 0 1 4 0
  .p2align 4
  .cv_loc 0 1 5 0
  nop
.Lf4:
  .cv_loc 0 1 6 0
  jmp g
.Lf_end:

  fn g, .text$b
  retq

## A thunk.
  fn k, .text$c
  je g
  retq
.Lk_end:
  movq __imp_g@FUSED(%rip), %rax

## An inline site.
  fn i, .text$d
  .cv_func_id 2
  .cv_inline_site_id 3 within 2 inlined_at 1 20 0
  .cv_loc 2 1 20 0
  nop
  .cv_loc 3 1 30 0
  je g
  .rept 11
  nop
  .endr
  .cv_loc 3 1 31 0
  je g
  .rept 11
  nop
  .endr
  .cv_loc 3 1 32 0
  .rept 12
  nop
  .endr
  .p2align 4
  .cv_loc 3 1 33 0
  nop
  .cv_loc 2 1 21 0
  retq
.Li_end:
  movq __imp_g@FUSED(%rip), %rax

## A jne over a jmp.
  fn j, .text$f
  .cv_func_id 4
  .cv_loc 4 1 40 0
  jne .Lj1
  .cv_loc 4 1 41 0
  jmp .Lj2
.Lj1:
  .cv_loc 4 1 42 0
  nop
.Lj2:
  .cv_loc 4 1 43 0
  retq
.Lj_end:
  movq __imp_g@FUSED(%rip), %rax

## A range longer than a record holds, whose first record, of the format's
## greatest length, padding lengthens.
  fn big, .text$e
  je g
.Lbig1:
  nop
  .p2align 4
  .rept 0xf006
  nop
  .endr
.Lbig2:
  retq
.Lbig_end:
  movq __imp_g@FUSED(%rip), %rax

## A procedure with separated code, and an inline site in both.
  fn c, .text$g
  .cv_func_id 5
  .cv_inline_site_id 6 within 5 inlined_at 1 50 0
  .cv_loc 5 1 50 0
  je .Lc1
  .cv_loc 6 1 60 0
.Lc1:
  nop
  .cv_loc 6 1 61 0
  je .Lc2
  .cv_loc 5 1 51 0
.Lc2:
  retq
.Lc_end:
  .section .text$h,"xr"
c.cold:
  .cv_loc 6 1 62 0
  je .Lc3
  .cv_loc 6 1 63 0
.Lc3:
  nop
  .cv_loc 5 1 52 0
  retq
.Lc_cold_end:

  .include "cv.inc"

  .section .debug$S,"dr"
  .long 241
  .long .Lthunk_end-.Lthunk
.Lthunk:
  .short .Lt1-.Lt0
.Lt0:
  .short 4354
  .long 0, 0, 0
  .secrel32 k
  .secidx k
  .short .Lk_end-k
  .byte 0
  .asciz "k"
  .p2align 2, 0
.Lt1:
  .short 2, 6
.Lthunk_end:
  .p2align 2, 0
  .cv_linetable 4, j, .Lj_end
  .cv_filechecksums
  .cv_stringtable

#--- a64.s
  .include "types.inc"
  .cv_file 1 "t.c"

  fn f, .text$a
  .cv_func_id 0
  .cv_loc 0 1 1 0
  adrp x0, cell
.Lf1:
  .cv_loc 0 1 2 0
  add x0, x0, :lo12:cell
.Lf2:
  .cv_loc 0 1 3 0
  bl g
.Lf3:
  .cv_loc 0 1 4 0
  .p2align 4
  .cv_loc 0 1 5 0
  nop
.Lf4:
  .cv_loc 0 1 6 0
  b g
.Lf_end:

  fn g, .text$b
  ret

  fn i, .text$d
  .cv_func_id 2
  .cv_inline_site_id 3 within 2 inlined_at 1 20 0
  .cv_loc 2 1 20 0
  nop
  .cv_loc 3 1 30 0
  adrp x0, cell
  add x0, x0, :lo12:cell
  nop
  nop
  .cv_loc 3 1 31 0
  nop
  .cv_loc 3 1 32 0
  nop
  nop
  .p2align 4
  .cv_loc 3 1 33 0
  nop
  .cv_loc 2 1 21 0
  ret
.Li_end:

  fn big, .text$e
  adrp x0, cell
  add x0, x0, :lo12:cell
.Lbig1:
  nop
  .p2align 4
  .rept 0x3bfe
  nop
  .endr
.Lbig2:
  ret
.Lbig_end:

  fn c, .text$g
  .cv_func_id 5
  .cv_inline_site_id 6 within 5 inlined_at 1 50 0
  .cv_loc 5 1 50 0
  adrp x0, cell
  add x0, x0, :lo12:cell
  .cv_loc 6 1 60 0
  nop
  .cv_loc 6 1 61 0
  adrp x0, cell
  add x0, x0, :lo12:cell
  .cv_loc 5 1 51 0
  ret
.Lc_end:
  .section .text$h,"xr"
c.cold:
  .cv_loc 6 1 62 0
  adrp x0, cell
  add x0, x0, :lo12:cell
  .cv_loc 6 1 63 0
  nop
  .cv_loc 5 1 52 0
  ret
.Lc_cold_end:
  .data
cell:
  .quad 0

  .include "cv.inc"
  .section .debug$S,"dr"
  .cv_filechecksums
  .cv_stringtable

#--- warn.s
  .include "types.inc"
  .section .text$a,"xr",discard,f
  .globl f
f:
  je g
  retq
.Lw_end:
  movq __imp_g@FUSED(%rip), %rax
  .section .text$b,"xr",discard,g
  .globl g
g:
  retq

  .section .debug$S,"dr"
  .long 4
  .long 241
  .long .Lsyms_end-.Lsyms
.Lsyms:
  .short .L1-.L0
.L0:
  .short 4396
  .short 0
  .short .Lw_end-f
  .secrel32 f
  .secrel32 g
  .secidx f
  .secidx g
.L1:
  proc f, .Lw_end
  .short .L5-.L4
.L4:
  .short 4445
  .long 0, 0
  .long 0x1002
  .long 0
  .byte 0x0b, 0x01, 0x04, 0x01
.L5:
  .short 2, 4430
  .short .L7-.L6
.L6:
  .short 4429
  .long 0, 0
  .long 0x1002
  .byte 0x02, 0x01, 0x04, 0x01
.L7:
  .short 2, 4430, 2, 6
.Lsyms_end:
  .long 245
  .long 4
  .secrel32 f

