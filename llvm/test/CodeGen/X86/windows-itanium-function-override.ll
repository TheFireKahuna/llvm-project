; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s \
; RUN:   | FileCheck %s --check-prefix=ASM --implicit-check-not=resolver
; RUN: llc -mtriple=x86_64-pc-windows-ntposix -filetype=obj < %s -o %t.o
; RUN: llvm-readobj --sections --section-data --relocs --symbols %t.o \
; RUN:   | FileCheck %s --check-prefix=OBJ --implicit-check-not=resolver

;; On Windows Itanium and NT-POSIX a multiversioned function is a stub: its
;; type prefix, then a jump to X, the version every processor the target
;; supports runs. Where each other version needs only features that Windows
;; numbers, sections in Microsoft's function-override convention, keyed on X,
;; hold a diagram that tests them in priority order and the versions it
;; selects among, and Windows rewrites the jump, and each call, as it maps
;; the image. A call of the function is direct, under -fno-plt too. Nothing
;; calls the resolver at run time, so it is not emitted and pulls in nothing.

; ASM-LABEL: caller:
; ASM:         callq wide
; ASM:         jmp local

;; AVX2 is within the baseline, so wide.avx2 is X and no version follows it.
;; ERMSB and FSRM are 0x000 and 0x001 and rank above AVX-512 F, 0x13e. A
;; test's false edge leads to the next version's first test and its true edge
;; to the version's next test or leaf; the last node keeps the stub's target.
; ASM:       .section .text,"xr",discard,wide
; ASM-NEXT:  .globl wide
; ASM-NEXT:  .def wide;
; ASM:       .p2align 4
; ASM-NEXT:  .zero 4,204
; ASM-NEXT:  __cfi_wide:
; ASM-NEXT:    {disp32} nopl 16(%rax)
; ASM-NEXT:    movl $12345678, %eax
; ASM-NEXT:  wide:
; ASM-NEXT:    jmp wide.avx2
; ASM-NEXT:  .globl wide.avx2_$fo_default$
; ASM-NEXT:  wide.avx2_$fo_default$ = wide.avx2
; ASM-NEXT:  .section wide.avx2_$fo_bdd$,"y",discard,wide.avx2_$fo$
; ASM-NEXT:  .globl wide.avx2_$fo$
; ASM-NEXT:  wide.avx2_$fo$:
; ASM-NEXT:    .long 1
; ASM-NEXT:    .long 48
; ASM-NEXT:    .short 3
; ASM-NEXT:    .short 1
; ASM-NEXT:    .long 0
; ASM-NEXT:    .short 3
; ASM-NEXT:    .short 2
; ASM-NEXT:    .long 1
; ASM-NEXT:    .short 2
; ASM-NEXT:    .short 2
; ASM-NEXT:    .long 0
; ASM-NEXT:    .short 5
; ASM-NEXT:    .short 4
; ASM-NEXT:    .long 318
; ASM-NEXT:    .short 4
; ASM-NEXT:    .short 4
; ASM-NEXT:    .long 1
; ASM-NEXT:    .quad 0
; ASM-NEXT:  .section wide.avx2_$fo_rvas$,"y",associative,wide.avx2_$fo$
; ASM-NEXT:    .long wide.erms-[[END0:.*]]
; ASM-NEXT:  [[END0]]:
; ASM-NEXT:    .long wide.avx512f-[[END1:.*]]
; ASM-NEXT:  [[END1]]:

;; A local function's stub and sections are local, and a function whose
;; versions all lie within the baseline is the stub alone.
; ASM-NEXT:  .text
; ASM-NEXT:  .def local;
; ASM:       local:
; ASM-NEXT:    jmp local.default
; ASM-NEXT:  local.default_$fo_default$ = local.default
; ASM-NEXT:  .section local.default_$fo_bdd$,"y",discard,local.default_$fo$
; ASM-NEXT:  local.default_$fo$:
; ASM:       narrow:
; ASM-NEXT:    jmp narrow.avx
; ASM-NOT:   narrow.avx_$fo

;; The sections are COMDATs with no other characteristic but MC's alignment,
;; as Microsoft's are, the candidates' associated with the diagram's. The
;; stub's jump has a 32-bit field whatever its distance, and a REL32 with no
;; addend names each candidate.
; OBJ:      Name: wide.avx2_$fo_bdd$
; OBJ:      Characteristics [ (0x101000)
; OBJ:      Name: wide.avx2_$fo_rvas$
; OBJ:      Characteristics [ (0x101000)
; OBJ:        0000: 00000000 00000000
; OBJ:      IMAGE_REL_LLVM_BRANCH32 wide.avx2
; OBJ:      Section ({{.*}}) wide.avx2_$fo_rvas$ {
; OBJ-NEXT:   0x0 IMAGE_REL_AMD64_REL32 wide.erms
; OBJ-NEXT:   0x4 IMAGE_REL_AMD64_REL32 wide.avx512f
; OBJ-NEXT: }
; OBJ:      Name: wide.avx2_$fo$
; OBJ:      StorageClass: External
; OBJ:      Name: wide.avx2_$fo_rvas$
; OBJ:      Selection: Associative
; OBJ-NEXT: AssocSection: wide.avx2_$fo_bdd$
; OBJ:      Name: local.default_$fo$
; OBJ:      StorageClass: Static

@wide = weak_odr ifunc i32 (), ptr @wide.resolver, !kcfi_type !2
@local = internal ifunc i32 (), ptr @local.resolver
@narrow = ifunc i32 (), ptr @narrow.resolver

declare i1 @test()

define weak_odr hidden ptr @wide.resolver() {
  %a = call i1 @test()
  %b = call i1 @test()
  %x = select i1 %a, ptr @wide.erms, ptr @wide.avx2
  %y = select i1 %b, ptr @wide.avx512f, ptr %x
  ret ptr %y
}

define internal ptr @local.resolver() {
  %a = call i1 @test()
  %x = select i1 %a, ptr @local.fsrs, ptr @local.default
  ret ptr %x
}

define hidden ptr @narrow.resolver() {
  ret ptr @narrow.avx
}

define hidden i32 @wide.avx512f() "fmv-features"="avx512f" {
  ret i32 3
}
define hidden i32 @wide.erms() "fmv-features"="ermsb,fsrm" {
  ret i32 2
}
define hidden i32 @wide.avx2() "fmv-features"="avx2" {
  ret i32 1
}
define internal i32 @local.fsrs() "fmv-features"="fsrs" {
  ret i32 1
}
define internal i32 @local.default() "fmv-features" {
  ret i32 0
}
define hidden i32 @narrow.avx() "fmv-features"="avx" {
  ret i32 0
}

define i32 @caller() {
  %a = call i32 @wide()
  %b = tail call i32 @local()
  ret i32 %b
}

!llvm.module.flags = !{!0, !1, !3}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 7, !"RtLibUseGOT", i32 1}
