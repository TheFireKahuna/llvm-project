// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --symbols --coff-link-records %t.o | \
// RUN:   FileCheck %s --implicit-check-not=Label
// RUN: llvm-objdump -d -j '.text$w' %t.o | FileCheck %s --check-prefix=DIS
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %s -o %t.n.o
// RUN: llvm-readobj -r --symbols --coff-link-records %t.n.o | \
// RUN:   FileCheck %s --implicit-check-not=Label

// On Windows Itanium and NT-POSIX the linker writes a direct call in five
// bytes over the load of a pointer and the call through its register, and may
// delete a jmp that ends a section. In a section where it moves code in no
// other way, the assembler resolves every distance, gives each jump the
// width that holds in every link, and states each value that spans such a
// site, or alignment after the first one, in the Spans group of the link
// records: no relocation, no label.
//
// A field of an instruction is REL8 or REL32, the latter with the bytes of
// immediate that follow it. A difference of two labels is its span type with
// the offset of the second. A field whose relocation names a temporary label
// of the section names the section's symbol instead: the linker maps the
// offset in an absolute field, and ADDEND32 states what any other field holds
// besides that offset. JUMP32 is a jmp or jcc that is long as assembled and

// CHECK:      Relocations [
// CHECK-NEXT:   Section ({{[0-9]+}}) .text {
// CHECK-NEXT:     0x1B IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_g
// CHECK-NEXT:     0x23 IMAGE_REL_LLVM_ALIGN - (484)
// CHECK-NEXT:     0x31 IMAGE_REL_LLVM_BRANCH32 f
// CHECK-NEXT:     0x47 IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_g
// CHECK-NEXT:     0x53 IMAGE_REL_LLVM_BRANCH_TAIL ext
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$w {
// CHECK-NEXT:     0x3 IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_g
// CHECK-NEXT:     0x72 IMAGE_REL_LLVM_ALIGN - (485)
// CHECK-NEXT:     0xFC IMAGE_REL_LLVM_ALIGN - (482)
// CHECK-NEXT:     0x10B IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$t {
// CHECK-NEXT:     0x4 IMAGE_REL_LLVM_BRANCH_TAIL ext
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$c {
// CHECK-NEXT:     0x1 IMAGE_REL_LLVM_BRANCH_TAIL .text
// CHECK-NEXT:     0x6 IMAGE_REL_LLVM_BRANCH32 .text
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .rdata {
// CHECK-NEXT:     0x1A IMAGE_REL_AMD64_REL32 .text
// CHECK-NEXT:     0x1E IMAGE_REL_AMD64_REL32 .text
// CHECK-NEXT:     0x22 IMAGE_REL_AMD64_ADDR64 .text
// CHECK-NEXT:     0x2A IMAGE_REL_AMD64_ADDR32NB .text
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .pdata {
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_ADDR32NB .text
// CHECK-NEXT:     0x4 IMAGE_REL_AMD64_ADDR32NB .text
// CHECK-NEXT:     0x8 IMAGE_REL_AMD64_ADDR32NB .xdata
// CHECK-NEXT:   }
// CHECK-NEXT: ]

// A section that states spans has a list of its own fields, empty where no
// field spans a site. je and jrcxz to the first load, the jmp before it and
// the first entry of the table span none; nor does the last difference in
// .rdata, of two labels before it.
// CHECK:          Kind: Sections (0x2)
// CHECK-NEXT:     ResizableSection: .text (
// CHECK-NEXT:     ResizableSection: .text$w (
// CHECK-NEXT:     ResizableSection: .text$t (
// CHECK-NEXT:   }
// CHECK-NEXT:   Group {
// CHECK-NEXT:     Kind: Spans (0x5)
// CHECK-NEXT:     Spans {
// CHECK-NEXT:       Field: .text (
// CHECK-NEXT:       Target: .text (
// CHECK-NEXT:       0xB REL32
// CHECK-NEXT:       0x11 REL32_1
// CHECK-NEXT:       0x22 REL8
// CHECK-NEXT:       0x40 IMAGE_REL_AMD64_SSPAN32 0x3C
// CHECK-NEXT:       0x53 JUMP32
// CHECK-NEXT:     }
// CHECK-NEXT:     Spans {
// CHECK-NEXT:       Field: .text$w (
// CHECK-NEXT:       Target: .text$w (
// CHECK-NEXT:       0xA JUMP32
// CHECK-NEXT:       0x81 REL8
// CHECK-NEXT:       0xFE JUMP32
// CHECK-NEXT:       0x104 REL32
// CHECK-NEXT:     }
// CHECK-NEXT:     Spans {
// CHECK-NEXT:       Field: .text$t (
// CHECK-NEXT:       Target: .text$t (
// CHECK-NEXT:       0x4 JUMP32
// CHECK-NEXT:     }
// Each epilogue of unwind data of version 2 is a distance from the function's
// end.
// CHECK-NEXT:     Spans {
// CHECK-NEXT:       Field: .xdata (
// CHECK-NEXT:       Target: .text (
// CHECK-NEXT:       0x6 IMAGE_REL_LLVM_SSPAN_EPILOGV2 0x3B
// CHECK-NEXT:       0x8 IMAGE_REL_LLVM_SSPAN_EPILOGV2 0x52
// CHECK-NEXT:     }
// CHECK-NEXT:     Spans {
// CHECK-NEXT:       Field: .text$c (
// CHECK-NEXT:       Target: .text (
// CHECK-NEXT:       0x1 ADDEND32 0
// CHECK-NEXT:       0x6 ADDEND32 0
// CHECK-NEXT:     }
// CHECK-NEXT:     Spans {
// CHECK-NEXT:       Field: .rdata (
// CHECK-NEXT:       Target: .text (
// CHECK-NEXT:       0x0 IMAGE_REL_AMD64_SSPAN32 0x0
// CHECK-NEXT:       0x4 IMAGE_REL_LLVM_SSPAN8 0x18
// CHECK-NEXT:       0x5 IMAGE_REL_LLVM_SSPAN16 0x18
// CHECK-NEXT:       0x7 IMAGE_REL_LLVM_SSPAN_ULEB 0x0
// CHECK-NEXT:       0x8 IMAGE_REL_LLVM_SSPAN_SLEB 0x30
// CHECK-NEXT:       0x12 IMAGE_REL_LLVM_SSPAN64 0x18
// CHECK-NEXT:       0x1A ADDEND32 4
// CHECK-NEXT:       0x1E ADDEND32 6
// CHECK-NEXT:     }
// CHECK-NEXT:     Spans {
// CHECK-NEXT:       Field: .rdata (
// CHECK-NEXT:       Target: .text$t (
// CHECK-NEXT:       0x2E IMAGE_REL_AMD64_SSPAN32 0x0
// CHECK-NEXT:     }
// CHECK-NEXT:   }

// The linker redoes the padding of an ALIGN where it has deleted bytes, so a
// jump across one is short only where it would reach across the most padding:
// the first is 117 bytes from its target with 17 of 31 bytes of padding, the
// second 122 with none of 3. The first reaches with less padding, so it is the
// linker's, as is the je, 131 bytes from its target with a load and its call
// between, of which the linker may take five. The jne has 145, and is long in
// every link.
// DIS:       9: e9 72 00 00 00      jmp 0x80
// DIS:      80: eb 7a               jmp 0xfc
// DIS:      fc: 0f 84 83 00 00 00   je 0x185
// DIS:     102: 0f 85 91 00 00 00   jne 0x199

  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
.Lbegin:
  .seh_proc f
  .seh_unwindversion 2
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  je .Lcall
  jrcxz .Lcall
  leaq .Lloop(%rip), %rax
  cmpl $1, .Lloop(%rip)
  jmp .Lhere
.Lhere:
.Lcall:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  jne .Lhere
  .p2align 4
.Lloop:
  callq f
  jne .Lloop
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  retq
.Ltable:
  .long .Lloop - .Ltable
  .long .Lcall - .Ltable
  movq __imp_g@LOAD_CALL(%rip), %r11
  callq *%r11
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  jmp ext
.Lend:
  .seh_endproc

  .section .text$w,"xr"
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  jmp .Lnear
  .fill 100, 1, 0x90
  .p2align 5
.Lnear:
  jmp .Lfar
  .fill 122, 1, 0x90
  .p2align 2
.Lfar:
  je .Lpair
  jne .Lout
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  .fill 116, 1, 0x90
.Lpair:
  .fill 20, 1, 0x90
.Lout:
  retq

  .section .text$t,"xr"
t:
  je .Lt
  nop
.Lt:
  jmp ext
.Ltend:

  .section .text$c,"xr"
  jmp .Lloop
  callq .Lcall

  .section .rdata,"dr"
  .long .Lloop - .Lbegin
  .byte .Lloop - .Lcall
  .short .Lend - .Lcall
  .uleb128 .Lloop - .Lbegin
  .sleb128 .Lcall - .Lloop
  .quad .Lloop - .Lcall
  .long .Lloop - .
  .long .Lcall - . + 2
  .quad .Lloop
  .long .Lloop@IMGREL
  .long .Ltend - t
  .long .Lhere - .Lbegin
