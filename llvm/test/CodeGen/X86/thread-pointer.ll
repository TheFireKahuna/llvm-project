; RUN: llc -mtriple=x86_64-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,SYSV
; RUN: opt -mtriple=x86_64-windows-itanium -passes=thread-pointer %s \
; RUN:   | llc | FileCheck %s --check-prefixes=CHECK,WIN64

; The owned thread-local model: an entry derives its thread pointer once,
; inline, from the client id through the TID map, the map's entry read by a
; fault probe; a function passed one forms the image's base once at the site
; the linker writes in the image's form, and reaches another image's
; thread-local through its record.

@own = dso_local thread_local global i32 0
@le = dso_local thread_local(localexec) global i32 0
@ext = external thread_local global i32
declare void @g() "thread-pointer-argument"="read"
declare void @unused() "thread-pointer-argument" "thread-pointer-unused"
declare ptr @llvm.threadlocal.address.p0(ptr)

; The derivation is raised out of the loop, its cold arm __tp_slow given the
; image's map word; a callee that never reads the thread pointer is passed
; none and is called at its plain entry, which leaves the register as it is.
; The probe's table is the function's own, under the C personality on
; NT-POSIX and on Windows Itanium the runtime's, which lets a search pass.
; CHECK-LABEL: entry:
; SYSV:        .seh_handler __gcc_personality_seh0, @unwind, @except
; WIN64:       .seh_handler __tp_personality, @unwind, @except
; CHECK:       movl %gs:72, %e[[TID:[a-z]+]]
; CHECK:       movq __imp___tp_map$val(%rip), %r[[MAP:[a-z]+]]
; CHECK-NEXT:  testq %r[[MAP]], %r[[MAP]]
; CHECK-NEXT:  je entry.cold
; CHECK:       # EH_LABEL
; CHECK-NEXT:  movl (%r[[MAP]],%r[[TID]]), %eax
; CHECK:       testb $48, %al
; CHECK-NEXT:  je entry.cold
; CHECK:       movl %eax, %ecx
; CHECK-NEXT:  andl $2097088, %ecx
; CHECK-NEXT:  andl $63, %eax
; CHECK-NEXT:  lea{{[lq]}} (%rax,%rcx,4), %r14
; CHECK-NEXT:  shlq $10, %r14
; CHECK-NEXT:  addq __imp___tp_base$val(%rip), %r14
; CHECK:       [[LOOP:.LBB0_[0-9]+]]: # %loop
; CHECK:       callq g$tp
; CHECK-NEXT:  callq unused
; CHECK:       jb [[LOOP]]
; CHECK:       entry.cold:
; SYSV:        leaq __imp___tp_map$val(%rip), %rdi
; WIN64:       leaq __imp___tp_map$val(%rip), %rcx
; CHECK-NEXT:  movq __imp___tp_slow@LOAD_CALL(%rip), %rax
; CHECK-NEXT:  callq *%rax
; CHECK-NEXT:  movq %rax, %r14
define void @entry(i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  call void @g()
  call void @unused()
  %i1 = add i32 %i, 1
  %c = icmp ult i32 %i1, %n
  br i1 %c, label %loop, label %done
done:
  ret void
}

; The base site, the wide form at its length, which names the thread
; pointer's register; an access adds its offset; a local-exec access is the
; thread pointer plus the variable's TPOFF.
; CHECK-LABEL: image$tp:
; CHECK:       [[SITE:.Ltmp[0-9]+]]:
; CHECK-NEXT:  .reloc [[SITE]], IMAGE_REL_LLVM_TLS_BASE
; CHECK-NEXT:  movq __imp___tp_register$init(%rip), %rax
; CHECK-NEXT:  movq (%r14,%rax), %rax
; CHECK-NEXT:  rorq $17, %rax
; CHECK-NEXT:  xorq __imp___tp_register$init+16(%rip), %rax
; CHECK-NEXT:  movl own@SECREL32(%rax), %eax
; CHECK-NEXT:  addl le@TPOFF(%r14), %eax
define i32 @image() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @own)
  %v = load i32, ptr %p
  %q = call ptr @llvm.threadlocal.address.p0(ptr @le)
  %w = load i32, ptr %q
  %s = add i32 %v, %w
  ret i32 %s
}

; A call that passes the thread pointer may come first.
; CHECK-LABEL: after_call$tp:
; CHECK-DAG:   callq g$tp
; CHECK-DAG:   .reloc {{.*}}, IMAGE_REL_LLVM_TLS_BASE
define i32 @after_call() "thread-pointer-argument" {
  call void @g()
  %p = call ptr @llvm.threadlocal.address.p0(ptr @own)
  %v = load i32, ptr %p
  ret i32 %v
}

; The record form: the record, the place word's place and form, the key
; through the importer's own import of __tp_key, the wide word decoded,
; the form's select, the variable's offset. Each instruction that reads a
; value the chain derived carries a FUSED_INDEX naming it: the place (1), the
; thread pointer plus it (2), the wide word and its decode (3), with ViaFirst
; (16) where it is the first register source; the access that adds the
; offset to the select's base names the register the direct arm read the
; thread pointer from, which still holds it (14, r14's encoding).
; CHECK-LABEL: record$tp:
; CHECK:       movq __imp_ext$tls@FUSED(%rip), %[[R:[a-z0-9]+]]
; CHECK:       movq (%[[R]]), %[[P:[a-z0-9]+]]
; CHECK:       .site_fact 8, 0
; CHECK-NEXT:  movq (%[[P]]), %[[PL:[a-z0-9]+]]
; CHECK:       .reloc {{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp_ext$tls
; CHECK-NEXT:  .site_fact 8, 1
; CHECK-NEXT:  leaq (%r14,%[[PL]]), %[[T:[a-z0-9]+]]
; CHECK:       .site_fact 8, 1
; CHECK-NEXT:  movq (%r14,%[[PL]]), %[[W:[a-z0-9]+]]
; CHECK-NEXT:  movq __imp___tp_key@FUSED(%rip), %[[K:[a-z0-9]+]]
; CHECK:       .site_fact 8, 3
; CHECK-NEXT:  rorq $17, %[[W]]
; CHECK:       .site_fact 8, 19
; CHECK-NEXT:  .Ltmp{{[0-9]+}}:
; CHECK-NEXT:  .reloc {{.*}}, IMAGE_REL_LLVM_FUSED_ACCESS, __imp___tp_key
; CHECK-NEXT:  xorq (%[[K]]), %[[W]]
; CHECK:       .site_fact 8, 0
; CHECK-NEXT:  cmpq $0, 8(%[[P]])
; CHECK:       .site_fact 8, 19
; CHECK:       .site_fact 8, 2
; CHECK-NEXT:  cmoveq %[[T]], %[[W]]
; CHECK:       movl 8(%[[R]]),
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  .site_fact 14, 14
; CHECK-NEXT:  movl (%[[W]],
define i32 @record() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @ext)
  %v = load i32, ptr %p
  ret i32 %v
}

; The access says it reads the thread pointer only where the register the
; direct arm read it from still holds it, and nothing keeps it there for the
; access: an entry's derivation here is overwritten by the key's pointer, so
; neither access says so and the frame saves no register for it.
; CHECK-LABEL: record_after_call:
; WIN64:       pushq %rsi
; WIN64:       pushq %rdi
; WIN64:       subq $40, %rsp
; WIN64:       .seh_endprologue
; CHECK:       leaq (%[[TP:[a-z0-9]+]],%r
; CHECK:       movq __imp___tp_key@FUSED(%rip), %[[TP]]
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  movl (%[[S:[a-z0-9]+]],
; CHECK:       callq unused
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  movl %e{{[a-z]+}}, (%[[S]],
define i32 @record_after_call() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @ext)
  %a = load i32, ptr %p
  call void @unused()
  %b = add i32 %a, 1
  store i32 %b, ptr %p
  ret i32 %a
}

; The chain's own wide load and select overwrite the register of an entry's
; derivation, which relaxation to $tpoff words deletes, so the access still
; names it (rax, 0).
; CHECK-LABEL: derived:
; CHECK:       .site_fact 8, 1
; CHECK-NEXT:  leaq (%rax,%[[PL:[a-z0-9]+]]), %[[T:[a-z0-9]+]]
; CHECK:       .site_fact 8, 1
; CHECK-NEXT:  movq (%rax,%[[PL]]), %rax
; CHECK:       cmoveq %[[T]], %rax
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  .site_fact 14, 0
; CHECK-NEXT:  movl (%rax,
define i32 @derived() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @ext)
  %v = load i32, ptr %p
  ret i32 %v
}

; The image's base formed after a record's access, of a variable named with no
; llvm.threadlocal.address, which the pass gives one.
; CHECK-LABEL: record_then_own$tp:
; CHECK:       movq __imp_ext$tls@FUSED(%rip)
; CHECK:       .reloc {{.*}}, IMAGE_REL_LLVM_TLS_BASE
; CHECK:       own@SECREL32
define i32 @record_then_own() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @ext)
  %v = load i32, ptr %p
  %w = load i32, ptr @own
  %s = add i32 %v, %w
  ret i32 %s
}

; A refusal derives at the head of the entry block and returns __tp_try's
; negative answer before the body runs.
; CHECK-LABEL: refuse:
; CHECK:       movl %gs:72
; CHECK:       refuse.cold:
; CHECK:       movq __imp___tp_try@LOAD_CALL(%rip), %rax
; CHECK-NEXT:  callq *%rax
; CHECK-NEXT:  movq %rax, %r14
; CHECK-NEXT:  testq %rax, %rax
; CHECK-NEXT:  jns
define i32 @refuse() "thread-pointer-refusal" {
  call void @g()
  ret i32 0
}

; CHECK-LABEL: probe:
; CHECK:       movq __imp___tp_probe@LOAD_CALL(%rip), %rax
; CHECK-NEXT:  callq *%rax
; CHECK-NEXT:  movq %rax, %r14
; CHECK-NEXT:  callq g$tp
define void @probe() "thread-pointer-probe" {
  call void @g()
  ret void
}

; CHECK-LABEL: none:
; CHECK:       movq __imp___tp_none(%rip), %r14
; CHECK-NEXT:  callq g$tp
define void @none() "no-thread-pointer" {
  call void @g()
  ret void
}

; Under a funclet personality, whose table holds no probe's landing, the cold
; arm answers every case.
; CHECK-LABEL: seh:
; CHECK-NOT:   %gs:72
; CHECK:       movq __imp___tp_slow@LOAD_CALL(%rip), %rax
; CHECK-NEXT:  callq *%rax
; CHECK-NEXT:  movq %rax, %r14
; CHECK:       callq g$tp
declare i32 @__C_specific_handler(...)
define void @seh() personality ptr @__C_specific_handler {
  call void @g()
  ret void
}
