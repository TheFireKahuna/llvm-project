; RUN: sed 's/^;NOPLT: //' %s > %t.noplt.ll
; DEFINE: %{x64} = llc -mtriple=x86_64-unknown-windows-itanium
; DEFINE: %{a64} = llc -mtriple=aarch64-pc-windows-ntposix
; DEFINE: %{form} = FileCheck %s --implicit-check-not=.refptr --check-prefixes
; RUN: %{x64} < %s | %{form}=X64,X64-PLT
; RUN: %{x64} < %t.noplt.ll | %{form}=X64,X64-NOPLT
; RUN: %{x64} -O0 < %t.noplt.ll | %{form}=X64-O0
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %t.noplt.ll | \
; RUN:   %{form}=X64,X64-NOPLT
; RUN: %{x64} -addrsig < %s | FileCheck %s --check-prefix=ADDRSIG
; RUN: llc -mtriple=x86_64-pc-windows-msvc -addrsig < %t.noplt.ll | \
; RUN:   FileCheck %s --check-prefix=MSVC
; RUN: %if aarch64-registered-target %{ %{a64} < %s | %{form}=A64,A64-PLT %}
; RUN: %if aarch64-registered-target %{ %{a64} < %t.noplt.ll | %{form}=A64,A64-NOPLT %}
; RUN: %if aarch64-registered-target %{ %{a64} -O0 < %t.noplt.ll | %{form}=A64-O0 %}

; only declares, or a variable whose definition another may replace, may be
; another image's unless it is dso_local, as on ELF. Its address is loaded
; from its import pointer, which the linker makes the address itself where the
; symbol is in the image, and there is no .refptr stub. A call binds through

declare void @plain()
declare void @nolazy() nonlazybind
declare dso_local void @local()
declare hidden void @hidden()
declare dllexport void @exported() nonlazybind
declare dllimport void @imported()
declare extern_weak void @weak()
declare void @take(ptr)
@var = external global i32
@local_var = external dso_local global i32
@exported_var = external dllexport global i32
@vague = linkonce_odr global i32 0
@tls = external thread_local global i32

define void @defined() {
  ret void
}

define void @calls() {
; X64-LABEL: calls:
; X64-PLT:        callq plain
; X64-NOPLT:      movq __imp_plain@LOAD_CALL(%rip), %rax
; X64-NOPLT-NEXT: callq *%rax
; X64-NEXT:       movq __imp_nolazy@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       callq local
; X64-NEXT:       callq hidden
; X64-NEXT:       callq exported
; X64-NEXT:       callq defined
; X64-NEXT:       movq __imp_imported@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-NEXT:       movq __imp_weak@LOAD_CALL(%rip), %rax
; X64-NEXT:       callq *%rax
; X64-O0-LABEL: calls:
; X64-O0:         movq __imp_plain@LOAD_CALL(%rip), %rax
; X64-O0-NEXT:    callq *%rax
; X64-O0:         callq local
; X64-O0:         movq __imp_weak@LOAD_CALL(%rip), %rax
; X64-O0-NEXT:    callq *%rax
; MSVC-LABEL: calls:
; MSVC:           callq plain
; MSVC-NEXT:      callq nolazy
; MSVC:           callq *.refptr.weak(%rip)
; A64-LABEL: calls:
; A64-PLT:        bl plain
; A64-NOPLT:      adrp x8, __imp_plain
; A64-NOPLT-NEXT: ldr x8, [x8, :fused_lo12:__imp_plain]
; A64:            adrp x8, __imp_nolazy
; A64:            bl local
; A64-NEXT:       bl hidden
; A64-NEXT:       bl exported
; A64-NEXT:       bl defined
; A64-NEXT:       adrp x8, __imp_imported
; A64:            adrp x8, __imp_weak
; A64-O0-LABEL: calls:
; A64-O0:         adrp x8, __imp_plain
; A64-O0:         bl local
; A64-O0:         adrp x8, __imp_weak
  call void @plain()
  call void @nolazy()
  call void @local()
  call void @hidden()
  call void @exported()
  call void @defined()
  call void @imported()
  call void @weak()
  ret void
}

; A jump through a RIP-relative pointer has no REX prefix, so that its
; relocation states a jump the linker may make direct.
define void @tail_call() {
; X64-LABEL: tail_call:
; X64-PLT:        jmp plain
; X64-NOPLT:      {{^}} jmpq *__imp_plain(%rip)
; MSVC-LABEL: tail_call:
; MSVC:           jmp plain
; A64-LABEL: tail_call:
; A64-PLT:        b plain
; A64-NOPLT:      ldr x0, [x0, :fused_lo12:__imp_plain]
; A64-NOPLT:      br x0
  tail call void @plain()
  ret void
}

define void @library_call(ptr %d, ptr %s, i64 %n) {
; X64-LABEL: library_call:
; X64-PLT:        callq memcpy
; X64-NOPLT:      movq __imp_memcpy@LOAD_CALL(%rip), %rax
; X64-NOPLT-NEXT: callq *%rax
; X64-O0-LABEL: library_call:
; X64-O0:         movq __imp_memcpy@LOAD_CALL(%rip), %rax
; X64-O0-NEXT:    callq *%rax
; MSVC-LABEL: library_call:
; MSVC:           callq memcpy
; A64-LABEL: library_call:
; A64-PLT:        bl memcpy
; A64-NOPLT:      adrp x8, __imp_memcpy
; A64-O0-LABEL: library_call:
; A64-O0:         adrp x8, __imp_memcpy
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}

define void @addresses() {
; X64-LABEL: addresses:
; X64:            movq __imp_plain(%rip), %r{{cx|di}}
; X64:            movq __imp_weak(%rip), %r{{cx|di}}
; X64:            movq __imp_var(%rip), %r{{cx|di}}
; X64:            movq __imp_vague(%rip), %r{{cx|di}}
; X64:            leaq local(%rip), %r{{cx|di}}
; X64:            leaq hidden(%rip), %r{{cx|di}}
; X64:            leaq defined(%rip), %r{{cx|di}}
; X64:            leaq local_var(%rip), %r{{cx|di}}
; X64:            leaq exported_var(%rip), %r{{cx|di}}
; X64-O0-LABEL: addresses:
; X64-O0:         movq __imp_plain(%rip), %rcx
; X64-O0:         movq __imp_weak(%rip), %rcx
; X64-O0:         leaq local(%rip), %rcx
; MSVC-LABEL: addresses:
; MSVC:           leaq plain(%rip), %rcx
; MSVC:           movq .refptr.weak(%rip), %rcx
; MSVC:           leaq var(%rip), %rcx
; MSVC:           leaq vague(%rip), %rcx
; A64-LABEL: addresses:
; A64:            ldr x0, [x0, :lo12:__imp_plain]
; A64:            ldr x0, [x0, :lo12:__imp_weak]
; A64:            ldr x0, [x0, :lo12:__imp_var]
; A64:            ldr x0, [x0, :lo12:__imp_vague]
; A64:            add x0, x0, :lo12:local
; A64:            add x0, x0, :lo12:hidden
; A64:            add x0, x0, :lo12:defined
; A64:            add x0, x0, :lo12:local_var
; A64:            add x0, x0, :lo12:exported_var
; A64-O0-LABEL: addresses:
; A64-O0:         ldr x0, [x0, :lo12:__imp_plain]
; A64-O0:         add x8, x8, :lo12:local
  call void @take(ptr @plain)
  call void @take(ptr @weak)
  call void @take(ptr @var)
  call void @take(ptr @vague)
  call void @take(ptr @local)
  call void @take(ptr @hidden)
  call void @take(ptr @defined)
  call void @take(ptr @local_var)
  call void @take(ptr @exported_var)
  ret void
}

; A thread-local variable is never imported.
define i32 @tls_read() {
; X64-LABEL: tls_read:
; X64:            movl tls@SECREL32(%rax), %eax
; A64-LABEL: tls_read:
; A64:            ldr w0, [x8, :secrel_lo12:tls]
  %v = load i32, ptr @tls
  ret i32 %v
}

; The linker rewrites only the load of an import pointer the compiler chose,
; so that load is not folded into the instruction that uses it, a call or a
; tail call aside, nor narrowed. A known import's is, as on other targets.
define i32 @compare(ptr %p) {
; X64-LABEL: compare:
; X64:            movq __imp_plain(%rip), %rax
; X64-NEXT:       cmpq %rax, %r{{cx|di}}
; X64:            cmpq __imp_imported(%rip), %r{{cx|di}}
; MSVC-LABEL: compare:
; MSVC:           cmpq __imp_imported(%rip), %rcx
  %a = icmp eq ptr %p, @plain
  %b = icmp eq ptr %p, @imported
  %ab = or i1 %a, %b
  %r = zext i1 %ab to i32
  ret i32 %r
}

define i32 @narrow() {
; X64-LABEL: narrow:
; X64:            movq __imp_plain(%rip), %rax
; X64-NOT:        __imp_plain
; X64:            retq
  %a = ptrtoint ptr @plain to i64
  %t = trunc i64 %a to i32
  ret i32 %t
}

; The address-significance table names the pointer through which the code
; reaches a declaration, which adds no symbol, and a definition itself.
; ADDRSIG:      .addrsig_sym __imp_plain
; ADDRSIG:      .addrsig_sym __imp_weak
; ADDRSIG:      .addrsig_sym __imp_var
; ADDRSIG-NEXT: .addrsig_sym local_var
; ADDRSIG-NEXT: .addrsig_sym exported_var
; ADDRSIG-NEXT: .addrsig_sym vague
; MSVC:         .addrsig_sym plain
; MSVC:         .addrsig_sym weak
; MSVC:         .addrsig_sym var

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
;NOPLT: !llvm.module.flags = !{!0}
;NOPLT: !0 = !{i32 7, !"RtLibUseGOT", i32 1}
