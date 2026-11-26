; REQUIRES: x86, aarch64
; DEFINE: %{link} = lld-link -lldignoreenv -dll -noentry -nodefaultlib -opt:noref \
; DEFINE:   -failifmismatch:_WIN_ITANIUM_KCFI_PREFIX=00000010.0
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -function-sections -filetype=obj %s -o %t.x64.obj
; RUN: %{link} -machine:amd64 %t.x64.obj -out:%t.x64.dll -lldmap:%t.x64.map
; RUN: llvm-objdump -s -j .text %t.x64.dll | FileCheck %s --check-prefix=X64
; RUN: FileCheck %s --check-prefix=MAP < %t.x64.map
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -function-sections -filetype=obj %s -o %t.a64.obj
; RUN: %{link} -machine:arm64 %t.a64.obj -out:%t.a64.dll -lldmap:%t.a64.map
; RUN: llvm-objdump -s -j .text %t.a64.dll | FileCheck %s --check-prefix=A64
; RUN: FileCheck %s --check-prefix=MAP < %t.a64.map

;; The nops aligning an entry after its type prefix are left out of the image:
;; the section is placed as if they were there, overlapping what precedes it;
;; the first section of .text keeps its place.

;; a's return is where b's padding would be, d's where e's would be on
;; AArch64, and the gap before e on x86-64 is the linker's fill.
; X64:      180001000 {{........}} 0f1f8010 000000b8 4e61bc00
; X64-NEXT: 180001010 c3{{......}} 0f1f8010 000000b8 4e61bc00
; X64-NEXT: 180001020 c3cccccc cccccccc cccccccc cccccccc
; X64-NEXT: 180001030 07000000 0f1f8110 000000b8 4e61bc00
; X64-NEXT: 180001040 c3{{......}} 0f1f8010 000000b8 4e61bc00
; X64-NEXT: 180001050 48b88877 66554433 2211c3cc cccccccc
; X64-NEXT: 180001060 cccccccc 0f1f8010 000000b8 4e61bc00
; X64-NEXT: 180001070 c3

; A64:      180001000 {{........}} 0f1f8010 000000b8 4e61bc00
; A64-NEXT: 180001010 c0035fd6 0f1f8010 000000b8 4e61bc00
; A64-NEXT: 180001020 c0035fd6 00000000 00000000 00000000
; A64-NEXT: 180001030 07000000 0f1f8110 000000b8 4e61bc00
; A64-NEXT: 180001040 c0035fd6 0f1f8010 000000b8 4e61bc00
; A64:      180001060 c0035fd6 0f1f8010 000000b8 4e61bc00
; A64-NEXT: 180001070 c0035fd6

;; The map gives each section from its first byte in the image.
; MAP:      00001004 {{[0-9a-f]+ +16 .*}}:(.text)
; MAP-NEXT: 00001004 {{.*}} __cfi_a
; MAP-NEXT: 00001010 {{.*}} a
; MAP-NEXT: 00001014 {{[0-9a-f]+ +16 .*}}:(.text)
; MAP-NEXT: 00001014 {{.*}} __cfi_b
; MAP-NEXT: 00001020 {{.*}} b
; MAP-NEXT: 00001030 {{[0-9a-f]+ +16 .*}}:(.text)
; MAP-NEXT: 00001030 {{.*}} __cfi_c
; MAP-NEXT: 00001040 {{.*}} c
; MAP-NEXT: 00001044 {{[0-9a-f]+ +16 .*}}:(.text)
; MAP-NEXT: 00001044 {{.*}} __cfi_d
; MAP-NEXT: 00001050 {{.*}} d
; MAP-NEXT: 00001064 {{[0-9a-f]+ +16 .*}}:(.text)
; MAP-NEXT: 00001064 {{.*}} __cfi_e
; MAP-NEXT: 00001070 {{.*}} e

define void @a() !kcfi_type !2 {
  ret void
}

define void @b() !kcfi_type !2 {
  ret void
}

define void @c() !kcfi_type !2 !kcfi_vfn_type !3 {
  ret void
}

define i64 @d() !kcfi_type !2 {
  ret i64 1234605616436508552
}

define void @e() !kcfi_type !2 {
  ret void
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 7}
