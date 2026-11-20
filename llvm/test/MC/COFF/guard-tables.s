// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=X64=1 %s -o %t.x64.o
// RUN: llvm-objdump -t %t.x64.o > %t.x64.txt
// RUN: llvm-readobj -x '.gfids$y' -x '.giats$y' %t.x64.o >> %t.x64.txt
// RUN: FileCheck %s --input-file=%t.x64.txt
// RUN: %if aarch64-registered-target %{ llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj %s -o %t.a64.o %}
// RUN: %if aarch64-registered-target %{ llvm-objdump -t %t.a64.o > %t.a64.txt %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -x '.gfids$y' -x '.giats$y' %t.a64.o >> %t.a64.txt %}
// RUN: %if aarch64-registered-target %{ FileCheck %s --input-file=%t.a64.txt %}
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj \
// RUN:   -defsym=NONE=1 %s -o %t.none.o
// RUN: llvm-objdump -t %t.none.o > %t.none.txt
// RUN: llvm-readobj -x '.gfids$y' -x '.giats$y' %t.none.o >> %t.none.txt
// RUN: FileCheck %s --input-file=%t.none.txt --check-prefix=NONE
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj -defsym=X64=1 \
// RUN:   -defsym=MSVC=1 %s -o %t.m.o
// RUN: llvm-readobj --sections --symbols %t.m.o | FileCheck %s \
// RUN:   --check-prefix=MSVC --implicit-check-not='.g{{[fi]}}' \
// RUN:   --implicit-check-not=@feat.00

// An object for Windows Itanium or NT-POSIX lists in .gfids$y each function,
// and in .giats$y each import pointer, whose address one of its relocations
// takes, and sets bit 0x800 of @feat.00. A call or a jump, also through an
// import pointer, a branch, a field through which a function is only called
// and unwind data take no address. A symbol that has a type other than a
// function's is not listed; an undefined one without a type is, as it may be
// a function.

// CHECK-DAG: [{{ *}}[[#%d,CODE:]]]{{.*}} in_code{{$}}
// CHECK-DAG: [{{ *}}[[#%d,DATA:]]]{{.*}} in_data{{$}}
// CHECK-DAG: [{{ *}}[[#%d,WEAK:]]]{{.*}} in_weak{{$}}
// CHECK-DAG: [{{ *}}[[#%d,UNDEF:]]]{{.*}} undefined{{$}}
// CHECK-DAG: [{{ *}}[[#%d,IMP:]]]{{.*}} __imp_taken{{$}}
// CHECK-DAG: (sec -1){{.*}}(scl 3) (nx 0) 0x00000800 @feat.00
// CHECK:      Hex dump of section '.gfids$y':
// CHECK-NEXT: 0x00000000 [[#%.2x,CODE]]000000 [[#%.2x,DATA]]000000 [[#%.2x,WEAK]]000000 [[#%.2x,UNDEF]]000000 {{[^ ]+$}}
// CHECK-EMPTY:
// CHECK-NEXT: Hex dump of section '.giats$y':
// CHECK-NEXT: 0x00000000 [[#%.2x,IMP]]000000 {{[^ ]+$}}
// CHECK-EMPTY:

// NONE:       (sec -1){{.*}}(scl 3) (nx 0) 0x00004800 @feat.00
// NONE:       Hex dump of section '.gfids$y':
// NONE-EMPTY:
// NONE-NEXT:  Hex dump of section '.giats$y':
// NONE-EMPTY:
// NONE-NOT:   {{.}}

// MSVC: Name: .text

.ifdef NONE
  @feat.00 = 0x4000
.else
  .def in_code; .scl 2; .type 32; .endef
  .def in_data; .scl 2; .type 32; .endef
  .def in_weak; .scl 2; .type 32; .endef
  .def called; .scl 2; .type 32; .endef
  .text
  .globl in_code, in_data, called
  .weak in_weak
in_code:
in_data:
in_weak:
  ret
called:
  .seh_proc called
  .seh_endprologue
  ret
  .seh_endproc
.ifdef X64
  callq called
  jmp called
  leaq in_code(%rip), %rax
  leaq in_weak(%rip), %rax
  leaq undefined(%rip), %rax
  leaq object(%rip), %rax
  movq __imp_taken(%rip), %rax
  callq *__imp_called(%rip)
.ifndef MSVC
  movq __imp_called@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_called
  cs; cs; cs; cs
  callq *%rax
  movq __imp_jumped@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_jumped
  rex64 jmpq *%rax
  int3; int3; int3; int3
.endif
.else
  bl called
  b called
  adrp x0, in_code
  add x0, x0, :lo12:in_code
  adrp x0, in_weak
  adrp x0, undefined
  adrp x0, object
  adrp x0, __imp_taken
  ldr x0, [x0, :lo12:__imp_taken]
  adrp x8, __imp_called
  ldr x8, [x8, :fused_lo12:__imp_called]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_called
  blr x8
  adrp x16, __imp_jumped
  ldr x16, [x16, :fused_lo12:__imp_jumped]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_jumped
  br x16
.endif

  .data
  .globl object
object:
  .quad in_data
  .quad in_code
  .long called@IMGREL_CALL
  .long called@REL32_CALL
.endif
