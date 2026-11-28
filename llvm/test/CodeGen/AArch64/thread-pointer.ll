; RUN: llc -mtriple=aarch64-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,NTP
; RUN: opt -mtriple=aarch64-windows-itanium -passes=thread-pointer %s \
; RUN:   | llc | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-ntposix -tls-size=12 < %s | \
; RUN:   FileCheck %s --check-prefix=SMALL

; The owned thread-local model on AArch64: the derivation, the base site and
; the record form, as X86/thread-pointer.ll pins them on x86-64.

@own = dso_local thread_local global i32 0
@le = dso_local thread_local(localexec) global i32 0
@arr = dso_local thread_local global [4 x i32] zeroinitializer
@le2 = dso_local thread_local(localexec) global [2 x i32] zeroinitializer
@ext = external thread_local global i32
declare void @g() "thread-pointer-argument"="read"
declare void @unused() "thread-pointer-argument" "thread-pointer-unused"
declare ptr @llvm.threadlocal.address.p0(ptr)

; The derivation is raised out of the loop, its cold arm __tp_slow given the
; image's map word; a callee that never reads the thread pointer is passed
; none and is called at its plain entry, which leaves the register as it is.
; CHECK-LABEL: entry:
; CHECK:       adrp x[[MAP:[0-9]+]], __imp___tp_map$val
; CHECK-NEXT:  add x[[MAP]], x[[MAP]], :lo12:__imp___tp_map$val
; CHECK-NEXT:  ldr w[[TID:[0-9]+]], [x18, #72]
; CHECK-NEXT:  ldar x[[MAP]], [x[[MAP]]]
; CHECK:       cbz x[[MAP]],
; CHECK:       // EH_LABEL
; CHECK-NEXT:  ldr w[[E:[0-9]+]], [x[[MAP]], {{[wx]}}[[TID]]{{(, uxtw)?}}]
; CHECK:       tst w[[E]], #0x30
; CHECK:       {{ubfiz|lsl}} x[[C:[0-9]+]], x[[E]], #12
; CHECK:       ldr x[[B:[0-9]+]], [x[[B]], :lo12:__imp___tp_base$val]
; CHECK-NEXT:  and x[[C]], x[[C]], #0x1fffc0000
; CHECK-NEXT:  bfi x[[C]], x[[E]], #10, #6
; CHECK-NEXT:  add x28, x[[B]], x[[C]]
; CHECK:       %loop
; CHECK:       bl g$tp
; CHECK-NEXT:  bl unused
; CHECK:       entry.cold:
; CHECK:       adrp x0, __imp___tp_map$val
; CHECK-NEXT:  add x0, x0, :lo12:__imp___tp_map$val
; CHECK:       ldr x[[F:[0-9]+]], [x[[F]], :fused_lo12:__imp___tp_slow]
; CHECK:       blr x[[F]]
; CHECK-NEXT:  mov x28, x0
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
; CHECK:       .reloc {{.*}}, IMAGE_REL_LLVM_TLS_BASE
; CHECK-NEXT:  adrp x[[T:[0-9]+]], __imp___tp_register$init
; CHECK-NEXT:  ldr x[[T]], [x[[T]], :lo12:__imp___tp_register$init]
; CHECK-NEXT:  ldr x[[T]], [x28, x[[T]]]
; CHECK-NEXT:  adrp x[[K:[0-9]+]], __imp___tp_register$init+16
; CHECK-NEXT:  ldr x[[K]], [x[[K]], :lo12:__imp___tp_register$init+16]
; CHECK-NEXT:  eor x[[T]], x[[K]], x[[T]], ror #17
; CHECK-DAG:   add x[[T]], x[[T]], :secrel_hi12:own
; CHECK-DAG:   ldr w{{[0-9]+}}, [x[[T]], :secrel_lo12:own]
; NTP-DAG:     add x[[L:[0-9]+]], x28, :tprel_hi12:le
; NTP-DAG:     ldr w{{[0-9]+}}, [x[[L]], :tprel_lo12_nc:le]
; Under -mtls-size=12 an offset has no high part: the access takes the low
; part, checked to fit, as ELF's local-exec does, and an indexed access adds
; it once, in an add that states fact 15 as each part's add does.
; SMALL-LABEL: image$tp:
; SMALL-NOT:   hi12
; SMALL-DAG:   ldr w{{[0-9]+}}, [x28, :tprel_lo12:le]
; SMALL-DAG:   ldr w{{[0-9]+}}, [x{{[0-9]+}}, :dtprel_lo12:own]
; SMALL-LABEL: indexed$tp:
; SMALL-NOT:   hi12
; SMALL:       .site_fact 15, 0
; SMALL-NEXT:  add x[[I:[0-9]+]], x{{[0-9]+}}, :dtprel_lo12:arr+4
; SMALL-NEXT:  ldr w0, [x[[I]], x0, lsl #2]
define i32 @image() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @own)
  %v = load i32, ptr %p
  %q = call ptr @llvm.threadlocal.address.p0(ptr @le)
  %w = load i32, ptr %q
  %s = add i32 %v, %w
  ret i32 %s
}

; An access at a constant offset below 4096 carries it in both parts, the
; low part in its own immediate; a register-indexed access adds both parts,
; and an access in another block folds the low part as well. Each part's add
; directly precedes the one instruction that reads it, and states fact 15
; where nothing after that reads what it wrote.
; CHECK-LABEL: field$tp:
; CHECK:       .site_fact 15, 0
; CHECK-NEXT:  add x[[F:[0-9]+]], x{{[0-9]+}}, :secrel_hi12:arr+12
; CHECK-NEXT:  ldr w[[V:[0-9]+]], [x[[F]], :secrel_lo12:arr+12]
; NTP-NEXT:    .site_fact 15, 0
; NTP-NEXT:    add x[[L:[0-9]+]], x28, :tprel_hi12:le2+4
; NTP-NEXT:    str w[[V]], [x[[L]], :tprel_lo12_nc:le2+4]
define void @field() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @arr)
  %f = getelementptr inbounds i8, ptr %p, i64 12
  %v = load i32, ptr %f
  %q = call ptr @llvm.threadlocal.address.p0(ptr @le2)
  %g = getelementptr inbounds i8, ptr %q, i64 4
  store i32 %v, ptr %g
  ret void
}

; CHECK-LABEL: indexed$tp:
; CHECK:       .site_fact 15, 0
; CHECK-NEXT:  add x[[I:[0-9]+]], x{{[0-9]+}}, :secrel_hi12:arr+4
; CHECK-NEXT:  .site_fact 15, 0
; CHECK-NEXT:  add x[[I]], x[[I]], :secrel_lo12:arr+4
; CHECK-NEXT:  ldr w0, [x[[I]], x0, lsl #2]
define i32 @indexed(i64 %i) "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @arr)
  %f = getelementptr inbounds i8, ptr %p, i64 4
  %e = getelementptr inbounds i32, ptr %f, i64 %i
  %v = load i32, ptr %e
  ret i32 %v
}

; NTP-LABEL: blocks$tp:
; NTP-NOT:     .site_fact 15
; NTP:         add x[[B:[0-9]+]], x{{[0-9]+}}, :secrel_hi12:own
; NTP-NEXT:    ldr w{{[0-9]+}}, [x[[B]], :secrel_lo12:own]
; NTP:         str wzr, [x[[B]], :secrel_lo12:own]
define i32 @blocks(i1 %c) "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @own)
  %v = load i32, ptr %p
  br i1 %c, label %clear, label %done
clear:
  store i32 0, ptr %p
  br label %done
done:
  ret i32 %v
}

; A call that passes the thread pointer may come first.
; CHECK-LABEL: after_call$tp:
; CHECK-DAG:   bl g$tp
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
; thread pointer plus it (2), the wide word and its decode (3), the key (4)
; and the form (5), with ViaFirst (16) where it is the first register source.
; CHECK-LABEL: record$tp:
; CHECK:       ldr x[[R:[0-9]+]], [x[[R]], :fused_lo12:__imp_ext$tls]
; CHECK:       ldr x[[P:[0-9]+]], [x[[R]]]
; CHECK:       ldr w[[O:[0-9]+]], [x[[R]], #8]
; CHECK:       .site_fact 8, 0
; CHECK-NEXT:  ldp x[[PL:[0-9]+]], x[[FORM:[0-9]+]], [x[[P]]]
; CHECK:       ldr x[[K:[0-9]+]], [x[[K]], :fused_lo12:__imp___tp_key]
; CHECK:       ldr x[[K]], [x[[K]]]
; CHECK:       .site_fact 8, 1
; CHECK-NEXT:  add x[[T:[0-9]+]], x28, x[[PL]]
; CHECK:       .site_fact 8, 5
; CHECK-NEXT:  cmp x[[FORM]], #0
; CHECK:       .site_fact 8, 2
; CHECK-NEXT:  ldr x[[W:[0-9]+]], [x[[T]]]
; CHECK:       .site_fact 8, 3
; CHECK:       .reloc {{.*}}, IMAGE_REL_LLVM_FUSED_INDEX, __imp___tp_key
; CHECK-NEXT:  .site_fact 8, 20
; CHECK-NEXT:  eor x[[D:[0-9]+]], x[[K]], x[[W]], ror #17
; CHECK:       .site_fact 8, 2
; CHECK:       .site_fact 8, 19
; CHECK-NEXT:  csel x[[S:[0-9]+]], x[[D]], x[[T]], ne
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  .site_fact 14, 28
; CHECK-NEXT:  ldr w0, [x[[S]], x[[O]]]
define i32 @record() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @ext)
  %v = load i32, ptr %p
  ret i32 %v
}

; The access says it reads the thread pointer only where the register the
; direct arm read it from still holds it, and nothing keeps it there for the
; access: an entry's derivation, in a register the call clobbers, is held at
; the first access and not at the second, and the frame saves no register for
; it.
; CHECK-LABEL: record_after_call:
; CHECK:       stp x19, x20, [sp, #-32]!
; CHECK-NEXT:  .seh_save_regp_x x19, 32
; CHECK-NEXT:  stp x21, x30, [sp, #16]
; CHECK-NEXT:  .seh_save_lrpair x21, 16
; CHECK-NEXT:  .seh_endprologue
; CHECK:       .site_fact 8, 1
; CHECK-NEXT:  add x{{[0-9]+}}, x[[TP:[0-9]+]], x
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  .site_fact 14, [[TP]]
; CHECK-NEXT:  ldr w{{[0-9]+}}, [x[[S:[0-9]+]], x[[O:[0-9]+]]]
; CHECK-NEXT:  bl unused
; CHECK:       .site_fact 8, 8
; CHECK-NEXT:  str w{{[0-9]+}}, [x[[S]], x[[O]]]
define i32 @record_after_call() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @ext)
  %a = load i32, ptr %p
  call void @unused()
  %b = add i32 %a, 1
  store i32 %b, ptr %p
  ret i32 %a
}

; A refusal derives at the head of the entry block and returns __tp_try's
; negative answer before the body runs.
; CHECK-LABEL: refuse:
; CHECK:       ldr w{{[0-9]+}}, [x18, #72]
; CHECK:       refuse.cold:
; CHECK:       ldr x[[F:[0-9]+]], [x[[F]], :fused_lo12:__imp___tp_try]
; CHECK:       blr x[[F]]
; CHECK:       tbnz x0, #63,
define i32 @refuse() "thread-pointer-refusal" {
  call void @g()
  ret i32 0
}

; CHECK-LABEL: probe:
; CHECK:       ldr x[[F:[0-9]+]], [x[[F]], :fused_lo12:__imp___tp_probe]
; CHECK:       blr x[[F]]
; CHECK-NEXT:  mov x28, x0
; CHECK-NEXT:  bl g$tp
define void @probe() "thread-pointer-probe" {
  call void @g()
  ret void
}

; CHECK-LABEL: none:
; CHECK:       adrp x28, __imp___tp_none
; CHECK-NEXT:  ldr x28, [x28, :lo12:__imp___tp_none]
; CHECK-NEXT:  bl g$tp
define void @none() "no-thread-pointer" {
  call void @g()
  ret void
}
