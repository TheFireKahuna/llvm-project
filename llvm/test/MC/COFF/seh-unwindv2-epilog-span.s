// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --unwind --coff-link-records %t.o | FileCheck %s --check-prefixes=CHECK,FORK
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %s -o %t.n.o
// RUN: llvm-readobj -r --unwind --coff-link-records %t.n.o | FileCheck %s --check-prefixes=CHECK,FORK
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.m.o
// RUN: llvm-readobj -r --unwind %t.m.o | FileCheck %s --check-prefixes=CHECK,MSVC \
// RUN:   --implicit-check-not=IMAGE_REL_LLVM
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | FileCheck %s --check-prefix=ASM
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc --defsym ERR=1 %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ERR

// On Windows Itanium and NT-POSIX each distance of a version 2 epilogue from
// its function's end is a relocation naming a label at the end and, in its
// PAIR, one at the epilogue, so the linker may resize the section. The
// function's last epilogue is listed last there, where a jump the linker
// deletes after an epilogue of that jump alone leaves the distance zero, which
// ends the list. `.seh_stackalloc` with `large` states an allocation in the
// large form, which the unwinder never takes for a pop.

// FORK:      Section ({{[0-9]+}}) .text {
// FORK-NEXT:   0x{{[0-9A-F]+}} IMAGE_REL_LLVM_JUMP8 $L
// FORK-NEXT:   0x{{[0-9A-F]+}} IMAGE_REL_LLVM_JUMP8 $L
// FORK-NEXT:   0x{{[0-9A-F]+}} IMAGE_REL_LLVM_BRANCH_TAIL ext
// FORK-NEXT:   0x{{[0-9A-F]+}} IMAGE_REL_LLVM_JUMP8 $L
// FORK-NEXT: }
// FORK-NEXT: Section ({{[0-9]+}}) .xdata {
// FORK-NEXT:   0x6 IMAGE_REL_LLVM_SSPAN_EPILOGV2 [[END1:\$L[0-9a-f]+]]
// FORK-NEXT:   0x6 IMAGE_REL_AMD64_PAIR [[MID:\$L[0-9a-f]+]]
// FORK-NEXT:   0x8 IMAGE_REL_LLVM_SSPAN_EPILOGV2 [[END1]]
// FORK-NEXT:   0x8 IMAGE_REL_AMD64_PAIR [[FIRST:\$L[0-9a-f]+]]
// FORK-NEXT:   0xA IMAGE_REL_LLVM_SSPAN_EPILOGV2 [[END1]]
// FORK-NEXT:   0xA IMAGE_REL_AMD64_PAIR [[LAST:\$L[0-9a-f]+]]
// FORK-NEXT:   0x16 IMAGE_REL_LLVM_SSPAN_EPILOGV2 [[END2:\$L[0-9a-f]+]]
// FORK-NEXT:   0x16 IMAGE_REL_AMD64_PAIR $L
// FORK-NEXT:   0x18 IMAGE_REL_LLVM_SSPAN_EPILOGV2 [[END2]]
// FORK-NEXT:   0x18 IMAGE_REL_AMD64_PAIR [[END2]]
// FORK-NEXT: }

// CHECK:      StartAddress: three
// CHECK:        Version: 2
// CHECK:        UnwindCodeCount: 6
// CHECK:          EPILOG atend=no, length=0x2
// FORK-NEXT:      EPILOG offset=0xC
// FORK-NEXT:      EPILOG offset=0x14
// FORK-NEXT:      EPILOG offset=0x6
// MSVC-NEXT:      EPILOG offset=0x6
// MSVC-NEXT:      EPILOG offset=0xC
// MSVC-NEXT:      EPILOG offset=0x14
// CHECK-NEXT:     ALLOC_SMALL size=32
// CHECK-NEXT:     PUSH_NONVOL reg=RBX
// CHECK-NEXT:   ]

// The second epilogue of `gone` lies at its end.
// CHECK:      StartAddress: gone
// CHECK:        UnwindCodeCount: 7
// CHECK:          EPILOG atend=no, length=0x1
// FORK-NEXT:      EPILOG offset=0x5
// FORK-NEXT:      EPILOG padding
// MSVC-NEXT:      EPILOG padding
// MSVC-NEXT:      EPILOG offset=0x5
// CHECK-NEXT:     EPILOG padding
// CHECK-NEXT:     ALLOC_SMALL size=40
// CHECK-NEXT:     ALLOC_LARGE size=8
// CHECK-NEXT:   ]

// FORK:      LinkRecords {
// FORK:          ResizableSection: .text

// ASM: .seh_stackalloc 8, large
// ASM: .seh_stackalloc 40{{$}}

  .text
  .globl three
  .seh_proc three
three:
  .seh_unwindversion 2
  pushq %rbx
  .seh_pushreg %rbx
  subq $32, %rsp
  .seh_stackalloc 32
  .seh_endprologue
  je .Lmid
  .seh_startepilogue
  addq $32, %rsp
  .seh_unwindv2start
  popq %rbx
  .seh_endepilogue
  retq
.Lmid:
  jne .Llast
  .seh_startepilogue
  addq $32, %rsp
  .seh_unwindv2start
  popq %rbx
  .seh_endepilogue
  retq
.Llast:
  .seh_startepilogue
  addq $32, %rsp
  .seh_unwindv2start
  popq %rbx
  .seh_endepilogue
  jmp ext
  .seh_endproc

  .globl gone
  .seh_proc gone
gone:
  .seh_unwindversion 2
  subq $8, %rsp
  .seh_stackalloc 8, large
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  je .Lgone
  .seh_startepilogue
  addq $48, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  retq
.Lgone:
  .seh_startepilogue
  addq $48, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  .seh_endproc

.ifdef ERR
  .seh_proc bad
bad:
  subq $8, %rsp
// ERR: [[#@LINE+1]]:{{[0-9]+}}: error: expected 'large'
  .seh_stackalloc 8, small
  .seh_endprologue
  .seh_endproc
.endif
