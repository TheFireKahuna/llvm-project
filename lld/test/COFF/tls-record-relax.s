# REQUIRES: x86, aarch64

## A record chain of a thread-local the image defines takes the image's form
## (spec section 15.1): what the base site of that form computes, and the
## variable's and its guard's offsets as the accesses' displacements. The
## form word's test, the select and the arm the form does not take are dead,
## and so is the key where the form is direct; a wide image's decode reads
## its own key word. Dead code goes where the assembler kept no size and is
## otherwise no-operations; a select whose destination is not the arm kept
## is a mov of it. An x86-64 function whose code keeps its size reads the
## record, which the image then holds. An image where every chain relaxes
## imports no key, except a wide DLL, whose fill writes its key word from it.

# IMPORTS: Name: ntposix.dll
# KEY:     Symbol: __tp_key
# IMPORTS: Symbol: __tp_register
# NONE:    Format:

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-ntposix
# DEFINE: %{link} = lld-link -machine:%{a} -debug:symtab fill.obj ntposix.lib \
# DEFINE:   tls.obj use.obj
# DEFINE: %{dis} = llvm-objdump -d --no-show-raw-insn
# DEFINE: %{imports} = llvm-readobj --coff-imports
# DEFINE: %{run} = \
# DEFINE:   lld-link -machine:%{a} -def:np.def -out:ntposix.lib && \
# DEFINE:   %{mc} %S/Inputs/fill-entry.s -o fill.obj && %{mc} tls.s -o tls.obj && \
# DEFINE:   %{mc} big.s -o big.obj && %{mc} exe.s -o exe.obj && \
# DEFINE:   %{mc} %S/Inputs/tls-chain-%{t}.s -o use.obj && \
# DEFINE:   %{link} -dll -noentry -out:direct.dll && \
# DEFINE:   %{dis} direct.dll | FileCheck --check-prefix=DIRECT-%{t} %s && \
# DEFINE:   %{imports} direct.dll | FileCheck --check-prefix=IMPORTS %s \
# DEFINE:     --implicit-check-not=__tp_key && \
# DEFINE:   %{link} big.obj -dll -noentry -out:wide.dll && \
# DEFINE:   %{dis} wide.dll | FileCheck --check-prefix=WIDE-%{t} %s && \
# DEFINE:   %{imports} wide.dll | FileCheck --check-prefixes=IMPORTS,KEY %s && \
# DEFINE:   %{link} exe.obj -entry:use$tp -subsystem:console -out:place.exe && \
# DEFINE:   %{dis} place.exe | FileCheck --check-prefix=PLACE-%{t} %s && \
# DEFINE:   %{imports} place.exe | FileCheck --check-prefix=NONE %s \
# DEFINE:     --implicit-check-not=__tp_key && \
# DEFINE:   %{link} exe.obj big.obj -entry:use$tp -subsystem:console -out:wide.exe && \
# DEFINE:   %{dis} wide.exe | FileCheck --check-prefix=EXE-WIDE-%{t} %s && \
# DEFINE:   %{imports} wide.exe | FileCheck --check-prefix=NONE %s \
# DEFINE:     --implicit-check-not=__tp_key && \
# DEFINE:   %{mc} %S/Inputs/tls-chain2-%{t}.s -o two.obj && \
# DEFINE:   lld-link -machine:%{a} -debug:symtab fill.obj ntposix.lib tls.obj \
# DEFINE:     two.obj -dll -noentry -out:two.dll && \
# DEFINE:   %{dis} two.dll | FileCheck --check-prefix=TWO-%{t} %s && \
# DEFINE:   %{imports} two.dll | FileCheck --check-prefix=IMPORTS %s \
# DEFINE:     --implicit-check-not=__tp_key && \
# DEFINE:   lld-link -machine:%{a} -debug:symtab fill.obj ntposix.lib tls.obj \
# DEFINE:     big.obj two.obj -dll -noentry -out:two-wide.dll && \
# DEFINE:   %{dis} two-wide.dll | FileCheck --check-prefix=TWO-WIDE-%{t} %s && \
# DEFINE:   sed '0,/site_fact 9, 0/{/site_fact 9, 0/d}' %S/Inputs/tls-chain-%{t}.s | \
# DEFINE:     %{mc} -o short.obj && \
# DEFINE:   lld-link -machine:%{a} -debug:symtab fill.obj ntposix.lib tls.obj \
# DEFINE:     short.obj -dll -noentry -out:short.dll && \
# DEFINE:   %{dis} short.dll | FileCheck --check-prefix=SHORT-%{t} %s && \
# DEFINE:   %{imports} short.dll | FileCheck --check-prefixes=IMPORTS,KEY %s

# RUN: %{run}
# RUN: sed -e 's/__imp___tp_key@FUSED/__imp___tp_key/' \
# RUN:   -e '/FUSED_ACCESS, __imp___tp_key/d' %S/Inputs/tls-chain-x86_64.s | \
# RUN:   %{mc} -o shared.obj
# RUN: lld-link -machine:x64 -debug:symtab fill.obj ntposix.lib tls.obj \
# RUN:   shared.obj -dll -noentry -out:shared.dll
# RUN: %{dis} shared.dll | FileCheck --check-prefix=SHARED %s
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## A load of the place word's address that indexes alone do not read gives
## it to a chain the compiler did not state, which reads the record, the key
## and the form as compiled: every chain of the record stays.
# SHORT-x86_64:      <use$tp>:
# SHORT-x86_64-NEXT: leaq {{.*}}(%rip), %rax
# SHORT-x86_64-NEXT: movq (%rax), %rcx
# SHORT-x86_64:      xorq (%rdi), %rdx
# SHORT-x86_64-NEXT: cmpq $0x0, 0x8(%rcx)
# SHORT-x86_64-NEXT: cmovneq %rdx, %rsi
# SHORT-aarch64:      <use$tp>:
# SHORT-aarch64:      ldr x9, [x8]
# SHORT-aarch64:      cmp x9, #0x0
# SHORT-aarch64:      csel x9, x11, x10, ne

## Chains that share the key's load mark it at the first decode alone: a
## decode that reads the key through a register no marker names is still the
## chain's, and the chain takes the image's form, the key's load staying.
# SHARED:      <use$tp>:
# SHARED-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x180003008
# SHARED-NEXT: leaq (%r14,%rdx), %rsi
# SHARED-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rdi # 0x180002000 <__imp___tp_key>
# SHARED-NEXT: cmpb $0x0, 0x8(%rsi)
# SHARED-NEXT: movl 0x4(%rsi), %eax

## Two chains that share the load of the place word's address and the key's
## both take the image's form; the cold part's load of the record's pointer
## is dead, and so is the key's where the form is direct, while a wide image
## loads its own key word for both decodes.
# TWO-x86_64:      <two$tp>:
# TWO-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x180003008
# TWO-x86_64-NEXT: leaq (%r14,%rdx), %rsi
# TWO-x86_64-NEXT: cmpb $0x0, 0x8(%rsi)
# TWO-x86_64-NEXT: je
# TWO-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x180003008
# TWO-x86_64-NEXT: leaq (%r14,%rdx), %rsi
# TWO-x86_64-NEXT: movl 0x4(%rsi), %eax
# TWO-x86_64-NEXT: retq
# TWO-x86_64-NEXT: callq {{.*}} <init>
# TWO-x86_64-NEXT: jmp
# TWO-WIDE-x86_64:      <two$tp>:
# TWO-WIDE-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x180003008
# TWO-WIDE-x86_64-NEXT: movq (%r14,%rdx), %rdx
# TWO-WIDE-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %r12 # 0x180003018
# TWO-WIDE-x86_64-NEXT: rorq $0x11, %rdx
# TWO-WIDE-x86_64-NEXT: xorq %r12, %rdx
# TWO-WIDE-x86_64:      movq (%r14,%rdx), %rdx
# TWO-WIDE-x86_64-NEXT: rorq $0x11, %rdx
# TWO-WIDE-x86_64-NEXT: xorq %r12, %rdx
# TWO-WIDE-x86_64-NEXT: movq %rdx, %rsi
# TWO-WIDE-x86_64-NEXT: movl 0x4(%rsi), %eax
# TWO-WIDE-x86_64-NEXT: retq
# TWO-WIDE-x86_64-NEXT: callq {{.*}} <init>
# TWO-WIDE-x86_64-NEXT: jmp
# TWO-aarch64:      <two$tp>:
# TWO-aarch64-NEXT: adrp x20, 0x180003000
# TWO-aarch64-NEXT: ldr x10, [x20, #0x8]
# TWO-aarch64-NEXT: add x10, x28, x10
# TWO-aarch64-NEXT: mov x9, x10
# TWO-aarch64-NEXT: ldrb w14, [x9, #0x8]
# TWO-aarch64-NEXT: cbz
# TWO-aarch64-NEXT: ldr x10, [x20, #0x8]
# TWO-aarch64-NEXT: add x10, x28, x10
# TWO-aarch64-NEXT: mov x9, x10
# TWO-aarch64-NEXT: ldr w0, [x9, #0x4]
# TWO-aarch64-NEXT: ret
# TWO-aarch64-NEXT: bl {{.*}} <init>
# TWO-aarch64-NEXT: b
# TWO-WIDE-aarch64:      <two$tp>:
# TWO-WIDE-aarch64:      ldr x21, [x11, #0x18]
# TWO-WIDE-aarch64:      eor x11, x21, x12, ror #17
# TWO-WIDE-aarch64:      ldr x10, [x20, #0x8]
# TWO-WIDE-aarch64-NEXT: add x10, x28, x10
# TWO-WIDE-aarch64-NEXT: ldr x12, [x10]
# TWO-WIDE-aarch64-NEXT: eor x11, x21, x12, ror #17
# TWO-WIDE-aarch64-NEXT: mov x9, x11
# TWO-WIDE-aarch64-NEXT: ldr w0, [x9, #0x4]

## A direct DLL loads its place at 0x3008 and adds the thread pointer, a
## direct executable adds its place, 0x140; x is at 4 and its guard at 8.
# DIRECT-x86_64:      <use$tp>:
# DIRECT-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x180003008
# DIRECT-x86_64-NEXT: leaq (%r14,%rdx), %rsi
# DIRECT-x86_64-NEXT: cmpb $0x0, 0x8(%rsi)
# DIRECT-x86_64-NEXT: movl 0x4(%rsi), %eax
# DIRECT-x86_64-NEXT: retq
# PLACE-x86_64:      <use$tp>:
# PLACE-x86_64-NEXT: leaq 0x140(%r14), %rsi
# PLACE-x86_64-NEXT: cmpb $0x0, 0x8(%rsi)
# PLACE-x86_64-NEXT: movl 0x4(%rsi), %eax
# PLACE-x86_64-NEXT: retq

## A wide DLL decodes the word at its place with its key at 0x3018; a wide
## executable its word at H, 0x100, with its key word.
# WIDE-x86_64:      <use$tp>:
# WIDE-x86_64-NEXT: movq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x180003008
# WIDE-x86_64-NEXT: movq (%r14,%rdx), %rdx
# WIDE-x86_64-NEXT: rorq $0x11, %rdx
# WIDE-x86_64-NEXT: xorq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x180003018
# WIDE-x86_64-NEXT: movq %rdx, %rsi
# WIDE-x86_64-NEXT: cmpb $0x0, 0x8(%rsi)
# WIDE-x86_64-NEXT: movl 0x4(%rsi), %eax
# WIDE-x86_64-NEXT: retq
# EXE-WIDE-x86_64:      <use$tp>:
# EXE-WIDE-x86_64-NEXT: movq 0x100(%r14), %rdx
# EXE-WIDE-x86_64-NEXT: rorq $0x11, %rdx
# EXE-WIDE-x86_64-NEXT: xorq 0x{{[0-9a-f]+}}(%rip), %rdx # 0x140003018
# EXE-WIDE-x86_64-NEXT: movq %rdx, %rsi

## On ARM64 the dead instructions go: the place and the key at their pages.
# DIRECT-aarch64:      <use$tp>:
# DIRECT-aarch64-NEXT: adrp x9, 0x180003000
# DIRECT-aarch64-NEXT: ldr x10, [x9, #0x8]
# DIRECT-aarch64-NEXT: add x10, x28, x10
# DIRECT-aarch64-NEXT: mov x9, x10
# DIRECT-aarch64-NEXT: ldrb w14, [x9, #0x8]
# DIRECT-aarch64-NEXT: ldr w0, [x9, #0x4]
# DIRECT-aarch64-NEXT: ret
# PLACE-aarch64:      <use$tp>:
# PLACE-aarch64-NEXT: add x10, x28, #0x140
# PLACE-aarch64-NEXT: mov x9, x10
# PLACE-aarch64-NEXT: ldrb w14, [x9, #0x8]
# PLACE-aarch64-NEXT: ldr w0, [x9, #0x4]
# WIDE-aarch64:      <use$tp>:
# WIDE-aarch64-NEXT: adrp x9, 0x180003000
# WIDE-aarch64-NEXT: ldr x10, [x9, #0x8]
# WIDE-aarch64-NEXT: adrp x11, 0x180003000
# WIDE-aarch64-NEXT: ldr x11, [x11, #0x18]
# WIDE-aarch64-NEXT: add x10, x28, x10
# WIDE-aarch64-NEXT: ldr x12, [x10]
# WIDE-aarch64-NEXT: eor x11, x11, x12, ror #17
# WIDE-aarch64-NEXT: mov x9, x11
# WIDE-aarch64-NEXT: ldrb w14, [x9, #0x8]
# EXE-WIDE-aarch64:      <use$tp>:
# EXE-WIDE-aarch64-NEXT: adrp x11, 0x140003000
# EXE-WIDE-aarch64-NEXT: ldr x11, [x11, #0x18]
# EXE-WIDE-aarch64-NEXT: add x10, x28, #0x100
# EXE-WIDE-aarch64-NEXT: ldr x12, [x10]
# EXE-WIDE-aarch64-NEXT: eor x11, x11, x12, ror #17
# EXE-WIDE-aarch64-NEXT: mov x9, x11

## The place's load would not fit the span from .Lk to .Lt once it grew, so
## the function keeps its size: the record at 0x2048, its place, x's offset
## and its guard's, and the chain as compiled, reading the image's own key
## word at 0x3018 in place of the key's import, which the image leaves out.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix fix.s -o fix.obj
# RUN: lld-link -machine:x64 -def:np.def -out:ntposix.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix %S/Inputs/fill-entry.s -o fill.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix tls.s -o tls.obj
# RUN: lld-link -machine:x64 -dll -noentry fill.obj ntposix.lib tls.obj fix.obj -out:fix.dll
# RUN: llvm-objdump -d --no-show-raw-insn -s -j .text -j .rdata fix.dll | FileCheck --check-prefix=FIX %s
# RUN: llvm-readobj --coff-imports fix.dll | FileCheck --check-prefix=IMPORTS %s \
# RUN:   --implicit-check-not=__tp_key
# FIX:      180002040 00000000 00000000 08300080 01000000
# FIX-NEXT: 180002050 04000000 08000000
# FIX:      leaq 0x{{[0-9a-f]+}}(%rip), %rax # 0x180002048
# FIX-NEXT: movq (%rax), %rcx
# FIX-NEXT: movq (%rcx), %rdx
# FIX:      leaq (%r14,%rdx), %rsi
# FIX:      leaq 0x{{[0-9a-f]+}}(%rip), %rdi # 0x180003018
# FIX:      cmovneq %rdx, %rsi

#--- np.def
LIBRARY ntposix.dll
EXPORTS
__tp_register
__tp_key DATA

#--- tls.s
        .section .tls$,"dw"
        .globl  x
        .long   0
x:      .long   5
guard:  .byte   0
        .link_fact x, 7, guard

#--- big.s
        .section .tls$z,"dw"
        .zero   2048

#--- exe.s
        .section .text,"xr",one_only,__llvm_fill_exe
        .globl  __llvm_fill_exe
__llvm_fill_exe:
        ret
        .rva    __llvm_fill_list
        .rva    __llvm_fill_target

#--- fix.s
        .text
        .globl  fix
fix:
        movq    __imp_x$tls@FUSED(%rip), %rax
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
        .site_fact 9, 0
        movq    (%rax), %rcx
.Lk:
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        movq    (%rcx), %rdx
        .fill   124, 1, 0x90
.Lt:
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        leaq    (%r14,%rdx), %rsi
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 1
        movq    (%r14,%rdx), %rdx
        movq    __imp___tp_key@FUSED(%rip), %rdi
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        rorq    $17, %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 19
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp___tp_key
        xorq    (%rdi), %rdx
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 0
        cmpq    $0, 8(%rcx)
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 18
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 3
        cmovneq %rdx, %rsi
        .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_x$tls
        .site_fact 9, 0
        movl    8(%rax), %eax
        .reloc ., IMAGE_REL_LLVM_FUSED_INDEX, __imp_x$tls
        .site_fact 8, 8
        movl    (%rsi,%rax), %eax
        retq
        .section .rdata,"dr"
        .byte   .Lt - .Lk
