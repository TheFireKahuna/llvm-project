; REQUIRES: x86, aarch64
; RUN: split-file %s %t
; DEFINE: %{triple} = x86_64-unknown-windows-itanium
; DEFINE: %{link} = \
; DEFINE:   llc -mtriple=%{triple} -filetype=obj %t/dll.ll -o %t.dll.obj && \
; DEFINE:   lld-link -dll -noentry %t.dll.obj -out:%t.dll -implib:%t.lib && \
; DEFINE:   llc -mtriple=%{triple} -filetype=obj %t/main.ll -o %t.obj && \
; DEFINE:   lld-link -entry:main -subsystem:console %t.obj %t.lib \
; DEFINE:     -out:%t.exe && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn %t.exe
; RUN: %{link} | FileCheck %s --check-prefix=X64
; REDEFINE: %{triple} = aarch64-unknown-windows-itanium
; RUN: %{link} | FileCheck %s --check-prefix=A64

; The compiler makes a call through a register that a load of an import
; pointer or of a stub's alone reaches a fused call, also where another
; instruction reads the register and the load stays one, and where a register
; still holds the pointer that a call would read from memory. The assembler
; has checked each against that load, and the image links with each call
; through its register. A call past another call reads the pointer for itself.
; The weak function's pointer is the image's own and holds zero, which its
; load becomes.

; X64:      movq $0x0, %rax
; X64-NEXT: movq %rax, {{.*}}(%rip)
; X64-NEXT: testq %rax, %rax
; X64-NEXT: je
; X64-NEXT: callq *%rax
; X64-NEXT: movq {{.*}}(%rip), %rax
; X64-NEXT: movq %rax, {{.*}}(%rip)
; X64-NEXT: callq *%rax
; X64-NEXT: callq *{{.*}}(%rip)

; A64:      ldr x8, [x8]
; A64-NEXT: str x8, [x19]
; A64-NEXT: cbz x8,
; A64-NEXT: blr x8
; A64-NEXT: ldr x8, 0x
; A64-NEXT: str x8, [x19]
; A64-NEXT: blr x8
; A64-NEXT: ldr x8, 0x
; A64-NEXT: blr x8

;--- dll.ll
define dllexport void @imported() {
  ret void
}

;--- main.ll
declare extern_weak void @weak()
declare dllimport void @imported()
@slot = global ptr null

define void @main() {
entry:
  store ptr @weak, ptr @slot
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit

call:
  call void @weak()
  br label %exit

exit:
  store ptr @imported, ptr @slot
  call void @imported()
  call void @imported()
  ret void
}
