# REQUIRES: x86, aarch64
# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{link} = lld-link -lldignoreenv -dll -noentry -nodefaultlib -opt:noref \
# DEFINE:   -failifmismatch:_WIN_ITANIUM_KCFI_PREFIX=00000010.0 -export:f
# RUN: llvm-mc -triple=x86_64-pc-windows-ntposix -filetype=obj x64.s -o x64.obj
# RUN: %{link} x64.obj -out:x64.dll -lldmap:x64.map
# RUN: llvm-objdump -s -j .text x64.dll | FileCheck %s --check-prefix=X64
# RUN: FileCheck %s --check-prefix=MAP < x64.map
# RUN: llvm-mc -triple=aarch64-pc-windows-ntposix -filetype=obj a64.s -o a64.obj
# RUN: %{link} -machine:arm64 a64.obj -out:a64.dll -lldmap:a64.map
# RUN: llvm-objdump -s -j .text a64.dll | FileCheck %s --check-prefix=A64
# RUN: FileCheck %s --check-prefix=MAP < a64.map

## The prefix of a plain entry with a second type and the offset of its
## thread-pointer entry, 0F 1F 83, is 20 bytes: the nops aligning its entry are
## left out, and the offset is f$tp less f, -0x20.

# X64:      180001000 b8020000 00c3cccc cccccccc 07000000
# X64-NEXT: 180001010 e0ffffff 0f1f8310 000000b8 4e61bc80
# A64:      180001000 40008052 c0035fd6 00000000 07000000
# A64-NEXT: 180001010 e0ffffff 0f1f8310 000000b8 4e61bc80

# MAP: 00001000 {{.*}} f$tp
# MAP: 0000100c {{.*}} __cfi_f
# MAP: 00001020 {{.*}} f

#--- x64.s
	.section .text,"xr",one_only,f$tp
	.globl f$tp
	.p2align 4
f$tp:	movl $2, %eax
	retq

	.section .text,"xr",one_only,f
	.globl f
	.p2align 4
	.fill 12, 1, 0x90
__cfi_f:
	.long 7
	.long f$tp - . - 16
	.byte 0x0f, 0x1f, 0x83, 0x10, 0, 0, 0, 0xb8
	.long 0x80BC614E
f:	movl $1, %eax
	retq
	.link_fact f, 0, 3

#--- a64.s
	.section .text,"xr",one_only,f$tp
	.globl f$tp
	.p2align 4
f$tp:	mov w0, #2
	ret

	.section .text,"xr",one_only,f
	.globl f
	.p2align 4
	.fill 3, 4, 0xd503201f
__cfi_f:
	.word 7
	.word f$tp - . - 16
	.byte 0x0f, 0x1f, 0x83, 0x10, 0, 0, 0, 0xb8
	.word 0x80BC614E
f:	mov w0, #1
	ret
	.link_fact f, 0, 3
