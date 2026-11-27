# REQUIRES: x86, aarch64

## A foreign x86-64 REL32 against an imported function is classified by
## decoding its function, from the start for a known compiler and otherwise
## from proven starts; unclassified is an error, no room for a load a warning.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -dll -noentry -opt:noref -verbose -errorlimit:0
# DEFINE: %{check} = FileCheck %s --implicit-check-not="reference to" \
# DEFINE:   -DPROOF="decoded from proven starts" \
# DEFINE:   -DSTART="decoded from the function's start" \
# DEFINE:   -DNO="is not classified:"

# RUN: lld-link -machine:x64 -def:imp.def -out:imp.lib
# RUN: %{mc} -triple=x86_64-windows-itanium fork.s -o fork.obj
# RUN: %{mc} -triple=x86_64-windows-msvc proof.s -o proof.obj
# RUN: %{mc} -triple=x86_64-windows-msvc marked.s -o marked.obj
# RUN: %{mc} -triple=x86_64-windows-msvc compiler.s -o compiler.obj
# RUN: %{mc} -triple=x86_64-windows-msvc cv.s -o cv.obj
# RUN: %{mc} -triple=x86_64-windows-msvc other.s -o other.obj
# RUN: not %{link} imp.lib fork.obj proof.obj -out:proof.dll 2>&1 \
# RUN:   | %{check} --check-prefix=PROVEN
# RUN: %{link} imp.lib fork.obj other.obj -out:other.dll 2>&1 \
# RUN:   | %{check} --check-prefix=OTHER
# RUN: not %{link} imp.lib fork.obj marked.obj -out:marked.dll 2>&1 \
# RUN:   | %{check} --check-prefix=MARKED
# RUN: %{link} imp.lib fork.obj compiler.obj -out:compiler.dll 2>&1 \
# RUN:   | %{check} --check-prefix=COMPILER
# RUN: not %{link} imp.lib fork.obj cv.obj -out:cv.dll 2>&1 \
# RUN:   | %{check} --check-prefix=CV

## Nothing is decoded in an image without such an object.
# RUN: %{link} imp.lib proof.obj -out:foreign.dll 2>&1 \
# RUN:   | %{check} --check-prefix=FOREIGN
# FOREIGN: Reading proof.obj

## Nor is a reference that keeps its thunk whatever it is: one to a function
## of a DLL that is loaded late.
# RUN: %{link} imp.lib fork.obj proof.obj -out:delay.dll -delayload:imp.dll \
# RUN:   2>&1 | %{check} --check-prefix=FOREIGN

# PROVEN:      proof.obj: the reference to case0 at .text+0x11 in tabled is a branch, [[PROOF]]
# PROVEN-NEXT: to case1 at {{.*}} in tabled takes its address, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in forms is a branch, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in forms is a branch, [[PROOF]]
# PROVEN-NEXT: to lea64 at {{.*}} in forms takes its address, [[PROOF]]
# PROVEN-NEXT: warning: proof.obj: the reference to lea32 at .text+0x48 in forms is another operand, [[PROOF]]
# PROVEN-NEXT: to load at {{.*}} in forms is another operand, [[PROOF]]
# PROVEN-NEXT: to vpermb at {{.*}} in forms is another operand, [[PROOF]]
# PROVEN-NEXT: to load at {{.*}} in forms is another operand, [[PROOF]]
# PROVEN-NEXT: to jcc at {{.*}} in forms is a branch, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in forms is a branch, [[PROOF]]
# PROVEN-NEXT: error: proof.obj: the reference to rep at .text+0x82 in odd [[NO]] a form of the instruction that is not canonical, [[PROOF]]; it may take the address of rep, which is imp.dll's; -ignore-function-address-equality:proof.obj keeps this image's own
# PROVEN-NEXT: to addr32 at {{.*}} in odd [[NO]] a form of the instruction that is not canonical, [[PROOF]]
# PROVEN-NEXT: to segment at {{.*}} in odd [[NO]] a form of the instruction that is not canonical, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in odd [[NO]] a form of the instruction that is not canonical, [[PROOF]]
# PROVEN-NEXT: to memjump at {{.*}} in odd is another operand, [[PROOF]]
# PROVEN-NEXT: to fsjump at {{.*}} in odd [[NO]] a form of the instruction that is not canonical, [[PROOF]]
# PROVEN-NEXT: to data16 at {{.*}} in wide [[NO]] an unknown or divergent encoding, [[PROOF]]
# PROVEN-NEXT: to resume at {{.*}} in guarded is a branch, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in lands is a branch, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in lands is a branch, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in midpad [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in midpad [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in midsite [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in midsite [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in fixed is a branch, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in fixed is a branch, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in sdata [[NO]] bytes that are no instruction, padding or marked data, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in sdata [[NO]] no proven path reaches it, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in cut [[NO]] bytes that are no instruction, padding or marked data, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in cut [[NO]] no proven path reaches it, [[PROOF]]
# PROVEN-NEXT: to pointed at {{.*}} in named is a branch, [[PROOF]]
# PROVEN-NEXT: to chained at {{.*}} in parent is a branch, [[PROOF]]
# PROVEN-NEXT: to planted at {{.*}} in plant [[NO]] no proven path reaches it, [[PROOF]]
# PROVEN-NEXT: to after at {{.*}} in plant [[NO]] bytes that are no instruction, padding or marked data, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in junk [[NO]] bytes that are no instruction, padding or marked data, [[PROOF]]
# PROVEN-NEXT: to planted at {{.*}} in nopped [[NO]] no proven path reaches it, [[PROOF]]
# PROVEN-NEXT: to jump at {{.*}} in nopped [[NO]] bytes that are no instruction, padding or marked data, [[PROOF]]
# PROVEN-NEXT: to die at {{.*}} in noret [[NO]] a relocation is not on an operand of the instruction that holds it, [[PROOF]]
# PROVEN-NEXT: to noreturn at {{.*}} in noret [[NO]] a relocation is not on an operand of the instruction that holds it, [[PROOF]]
# PROVEN-NEXT: to overlap at {{.*}} in overlaps [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to overlap at {{.*}} in straddles [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to index at {{.*}} in indexed [[NO]] no proven path reaches it, [[PROOF]]
# PROVEN-NEXT: to prologue at {{.*}} in prolog [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to symbol at {{.*}} in label [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to prologue at {{.*}} in coded [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to after at {{.*}} in ended [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to resume at {{.*}} in scoped [[NO]] two instructions, or one and data, overlap, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in handled is a branch, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in cold is a branch, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in kept [[NO]] no proven path reaches it, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in loose [[NO]] no proven path reaches it, [[PROOF]]
# PROVEN-NEXT: to call at {{.*}} in inner [[NO]] two instructions, or one and data, overlap, [[PROOF]]

# OTHER:      warning: other.obj: the reference to call at .text+0x1 is another operand, by its type
# OTHER-NEXT: warning: other.obj: the reference to call at .data+0x0 is data, by its type

# MARKED:      marked.obj: the reference to data at {{.*}} in masm is data, [[PROOF]]
# MARKED-NEXT: to marked at {{.*}} in masm is a branch, [[PROOF]]
# MARKED-NEXT: to into at {{.*}} in enters [[NO]] two instructions, or one and data, overlap

# COMPILER:      compiler.obj: the reference to lea64 at {{.*}} in compiled takes its address, [[START]]
# COMPILER-NEXT: to vpermb at {{.*}} in compiled is another operand, [[START]]
# COMPILER-NEXT: to call at {{.*}} in compiled is a branch, [[START]]
# COMPILER-NEXT: to data at {{.*}} in compiled is data, [[START]]

# CV:      cv.obj: the reference to described at .text+0x5 in clean takes its address, [[START]]
# CV-NEXT: to inlined at {{.*}} in dirty [[NO]] no proven path reaches it, [[PROOF]]
# CV-NEXT: to call at {{.*}} in holds is a branch, [[START]]
# CV-NEXT: to held at {{.*}} in holds is data, [[START]]
# CV-NEXT: to lined at {{.*}} in lines [[NO]] two instructions, or one and data, overlap, [[START]]
# CV-NEXT: to lined at {{.*}} in labelled [[NO]] two instructions, or one and data, overlap, [[START]]

# RUN: lld-link -machine:arm64 -def:imp.def -out:imp-a64.lib
# RUN: %{mc} -triple=aarch64-windows-itanium fork.s -o fork-a64.obj
# RUN: %{mc} -triple=aarch64-windows-msvc arm64.s -o arm64.obj
# RUN: %{link} imp-a64.lib fork-a64.obj arm64.obj -out:arm64.dll 2>&1 \
# RUN:   | %{check} --check-prefix=ARM64

# ARM64:      arm64.obj: the reference to call at .text+0x0 is a branch, by its type
# ARM64-NEXT: to lea64 at .text+0x4 takes its address, by its type
# ARM64-NEXT: to lea64 at .text+0x8 takes its address, by its type
# ARM64-NEXT: to load at .text+0xc is another operand, by its type
# ARM64-NEXT: to jcc at .text+0x10 is a branch, by its type

#--- imp.def
LIBRARY imp.dll
EXPORTS
  __C_specific_handler __gxx_personality_seh0 call jump jcc lea64 lea32 load
  vpermb memjump rep addr32 segment fsjump data16 case0 case1 resume planted
  after noreturn die data overlap index pointed chained prologue symbol
  marked into static described inlined held lined

#--- fork.s
## Its own references are typed: none is classified.
.text
.globl __delayLoadHelper2
__delayLoadHelper2:
  .long call@IMGREL_CALL
  .long 0

#--- other.s
## Only a REL32 in code is decoded, and no other field but a word of data
## has room for the address.
.text
.globl other
other:
  movl $call@IMGREL, %eax
  ret
.data
  .long call - .

#--- func.inc
.macro func name
  .def \name; .scl 2; .type 32; .endef
  .globl \name
  .p2align 4, 0xcc
\name:
.endm

#--- proof.s
## An object with no producer identity: function-typed symbols, unwind ranges
## and what else the object's tables prove.
.include "func.inc"
.text
## Places only distance tables name; the second, reached through the first,
## names a place in another section at an offset inside an instruction here.
func tabled
  leaq .Ltable(%rip), %rdx
  movslq (%rdx,%rcx,4), %rax
  addq %rdx, %rax
  jmpq *%rax
.Lcase0:
  jmp case0
.Lcase1:
  leaq .Ltable2(%rip), %rdx
  movslq (%rdx,%rcx,4), %rax
  addq %rdx, %rax
  jmpq *%rax
.Lcase2:
  leaq case1(%rip), %rax
  retq
  .section .rdata,"dr"
  .p2align 2
.Ltable:
  .long .Lcase0-.Ltable
  .long .Lcase1-.Ltable
.Ltable2:
  .long .Lcold-.Ltable2
  .long .Lcase2-.Ltable2
  .section .text$cold,"xr"
  nop
.Lcold:
  retq
  .text

func forms
  .seh_proc forms
  pushq %rbx
  .seh_pushreg %rbx
  subq $32, %rsp
  .seh_stackalloc 32
  .seh_endprologue
  callq call
## The field of a relocated branch does not hold its target.
  callq call+1
  leaq lea64(%rip), %rax
  leal lea32(%rip), %eax
  movl load(%rip), %eax
  vpermb vpermb(%rip), %zmm1, %zmm0
  lock incl load(%rip)
## A static symbol of an object that no producer identifies is a label.
here:
  jne jcc
  addq $32, %rsp
  popq %rbx
  jmp jump
  .byte 0x90, 0x66, 0x90, 0x0f, 0x1f, 0x00, 0xcc
  .seh_endproc
## The function ends with its unwind range.
  .byte 1, 2, 3

## Forms that are not canonical, in a function that is otherwise proven.
func odd
  .byte 0xf3
  callq rep
  addr32 leaq addr32(%rip), %rax
  .byte 0x2e
  leaq segment(%rip), %rax
  xbegin jump
  je .Lodd
  jmpq *memjump(%rip)
.Lodd:
  .byte 0x64
  jmpq *fsjump(%rip)

## A branch whose offset has 16 bits on some processors.
func wide
  .byte 0x66
  callq data16
  retq

## A place that only the scope of the C handler names.
func guarded
  .seh_proc guarded
  .seh_handler __C_specific_handler, @except
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
.Ltry:
  movl $0, (%rcx)
.Ltried:
  addq $40, %rsp
  retq
.Lresume:
  callq resume
  addq $40, %rsp
  retq
  .seh_handlerdata
  .long 1
  .long .Ltry@IMGREL, .Ltried@IMGREL, 1, .Lresume@IMGREL
  .text
  .seh_endproc

## Landing pads only Itanium call sites name; one inside a reached instruction
## or a site ending inside one is refused; unread or cut tables name nothing.
.macro landing name, pad, end, encoding=0x01, cut=0, entry=.uleb128
func \name
  .seh_proc \name
  .seh_handler __gxx_personality_seh0, @unwind, @except
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  callq call
1:addq $40, %rsp
  retq
2:jmp jump
  .seh_handlerdata
  .byte 0xff, 0xff, \encoding
  .uleb128 4f-3f-\cut
3:\entry 1b-5-\name
  \entry 5+\end
  \entry 2b+\pad-\name
  .uleb128 0
4:
  .text
  .seh_endproc
.endm
landing lands, 0, 0
landing midpad, -4, 0
landing midsite, 0, 1
landing fixed, 0, 0, 0x03, 0, .long
landing sdata, 0, 0, 0x0b, 0, .long
landing cut, 0, 0, 0x01, 1

## An accepted branch proves its target in another section; a taken address or
## unreached relocation proves nothing; a branch into a decoded one is refused.
func hot
  jne .Lreached
  retq
func takes
  leaq .Lheld(%rip), %rax
  retq
func stray
  retq
  jne .Lloose
func twice
  jne .Linner
  jne .Linner+1
  retq
  .section .text$away,"xr"
func cold
  int3
.Lreached:
  callq call
  retq
func kept
  int3
.Lheld:
  callq call
  retq
func loose
  int3
.Lloose:
  callq call
  retq
func inner
  int3
.Linner:
  movl $0x90909090, %eax
  callq call
  retq
  .text

## A place that only a word of data names.
func named
  retq
.Lpointed:
  jmp pointed
  .section .rdata,"dr"
  .quad .Lpointed
  .text

## A place that only the unwind information chained to it names.
func parent
  retq
.Lparent:
  jmp chained
func fragment
  nop
.Lfragment:
  retq
  .section .pdata,"dr"
  .long fragment@IMGREL, .Lfragment@IMGREL, .Lchain@IMGREL
  .section .xdata,"dr"
.Lchain:
  .byte 0x21, 0, 0, 0
  .long .Lparent@IMGREL, fragment@IMGREL, .Lchain@IMGREL
  .text

## The bytes of a lea after a jump, which no path reaches.
func plant
  jmp .Lover
  .byte 0x48, 0x8d, 0x05
  .long planted-.-4
.Lover:
  callq after
  retq

## Instructions that no path reaches, and a no-operation that holds a
## relocation.
func junk
  jmp .Ljunk
  xorl %eax, %eax
.Ljunk:
  jmp jump
func nopped
  jmp .Lnopped
  .byte 0x0f, 0x1f, 0x80
  .long planted-.-4
.Lnopped:
  jmp jump

## Data after a call that does not return.
func noret
  callq die
  .byte 0x01, 0x02
  .long noreturn-.-4

## A jump into an instruction.
func overlaps
  jne .Linto+1
.Linto:
  movl $0x90909090, %eax
  callq overlap
  retq

## An instruction decoded over one that a path reached before it.
func straddles
  je .Lunder
  jmp .Lunder+1
.Lunder:
  movl $0x90909090, %eax
  callq overlap
  retq

## A table in code that nothing marks.
func indexed
  leaq .Lindex(%rip), %rax
  retq
.Lindex:
  .long index-.Lindex

## A prologue that ends inside an instruction, and a symbol inside one.
func prolog
  subq $40, %rsp
  callq prologue
.Lprologue:
  addq $40, %rsp
  retq
  .section .pdata,"dr"
  .long prolog@IMGREL, .Lprologue@IMGREL, .Lunwind@IMGREL
  .section .xdata,"dr"
.Lunwind:
  .byte 1, 3, 0, 0
  .text
func label
  callq symbol
  retq
inside = label+1

## Unwind codes, ranges and scopes ending inside an instruction; another
## handler's data is not read as scopes.
func coded
  subq $40, %rsp
  callq prologue
.Lcoded:
  addq $40, %rsp
  retq
func ended
  callq after
  retq
.Lpart:
  nop
  movl $1, %eax
  retq
func scoped
  callq resume
.Lscoped:
  retq
func handled
  callq call
.Lhandled:
  retq
  .section .pdata,"dr"
  .long coded@IMGREL, .Lcoded@IMGREL, .Lcodes@IMGREL
  .long .Lpart@IMGREL, .Lpart+2@IMGREL, .Lnone@IMGREL
  .long scoped@IMGREL, .Lscoped@IMGREL, .Lscopes@IMGREL
  .long handled@IMGREL, .Lhandled@IMGREL, .Lother@IMGREL
  .section .xdata,"dr"
.Lcodes:
  .byte 1, 4, 1, 0
  .byte 3, 0x42
  .p2align 2
.Lnone:
  .byte 1, 0, 0, 0
.Lscopes:
  .byte 0x09, 0, 0, 0
  .long __C_specific_handler@IMGREL
  .long 1
  .long scoped@IMGREL, scoped+1@IMGREL, 1, 0
.Lother:
  .byte 0x09, 0, 0, 0
  .long die@IMGREL
  .long 1
  .long handled@IMGREL, handled+1@IMGREL, 1, handled+2@IMGREL
  .text

#--- marked.s
## An object of MASM: a static symbol in code begins data, up to the next
## symbol.
.include "func.inc"
"@comp.id" = 0x01038da7
.text
func masm
## MASM's symbol for a line table is static too.
first = masm
  leaq table(%rip), %rax
  jmp over
table:
  .long data-table
  .long 0
  .def over; .scl 6; .endef
over:
  callq marked
  retq

## A path into data.
func enters
  jmp static
static:
  callq into
  retq

#--- compiler.s
## An object of the C compiler: instructions from each function's start on,
## and a table after them.
.include "func.inc"
"@comp.id" = 0x01048da7
.text
func compiled
  jmp .Lover
  leaq lea64(%rip), %rax
  vpermb vpermb(%rip), %zmm1, %zmm0
.Lover:
  jmpq *%rax
  callq call
  int3
$LN1:
  .long data-$LN1

#--- cv.s
## An object that CodeView describes as a compiler's. A procedure whose frame
## has no inline assembly is instructions from its start on.
.include "func.inc"
.macro proc name, flags
  .short 2f-1f
1:.short 0x1110
  .long 0, 0, 0, \name\()_end-\name, 0, 0, 0
  .secrel32 \name
  .secidx \name
  .byte 0
  .asciz "\name"
2:.short 4f-3f
3:.short 0x1012
  .long 0, 0, 0, 0, 0
  .short 0
  .long \flags
4:.short 2, 0x114f
.endm

.text
func clean
  jmp .Lover
  leaq described(%rip), %rax
.Lover:
  retq
clean_end:
## The function ends with its procedure.
  .byte 0x06

func dirty
  jmp .Lover2
  leaq inlined(%rip), %rax
.Lover2:
  retq
dirty_end:

func holds
  callq call
  retq
.Lheld:
  .long held-.Lheld
holds_end:

func lines
  callq lined
  retq
lines_end:

func labelled
  callq lined
  retq
labelled_end:

.section .debug$S,"dr"
  .long 4
  .long 0xf1, .Lsymbols_end-.Lsymbols
.Lsymbols:
  .short 2f-1f
1:.short 0x113c
  .long 0
  .short 0xd0, 1, 0, 0, 0, 1, 0, 0, 0
  .asciz "cc"
2:
  proc clean, 0
  proc dirty, 8
  proc holds, 0
  .short 2f-1f
1:.short 0x110c
  .long 0
  .secrel32 .Lheld
  .secidx .Lheld
  .asciz "held"
2:
  proc lines, 0
  proc labelled, 0
  .short 2f-1f
1:.short 0x1105
  .secrel32 labelled+1
  .secidx labelled
  .byte 0
  .asciz "label"
2:
.Lsymbols_end:
  .p2align 2
  .long 0xf2, .Llines_end-.Llines
.Llines:
  .secrel32 lines
  .secidx lines
  .short 0
  .long lines_end-lines
  .long 0, 1, 20
  .long 1, 0x80000001
.Llines_end:

#--- arm64.s
.text
.globl _start
_start:
  bl call
  adrp x0, lea64
  add x0, x0, :lo12:lea64
  ldr w1, [x0, :lo12:load]
  b.eq jcc
  ret
