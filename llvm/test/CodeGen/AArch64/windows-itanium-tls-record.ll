; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=aarch64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck --check-prefix=O0 %s
; RUN: llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck --check-prefix=MSVC %s

; On Windows Itanium a thread-local variable that is not dso_local may be
; another image's, whether it is declared or defined weakly, and is reached
; through the record whose address the import pointer of <name>$tls holds: the
; address of the defining image's TLS index, then the variable's offset in
; that image's template. The loads from the record are described to the
; linker as accesses of the record's load, and the load of the address as a
; load itself, with the loads of the TLS index through it, and the load of the
; offset with the instructions that add it to another register, so that where
; the record is in the image the linker can take the index's page and the
; offset in their place. A dso_local variable is reached directly, and the
; MSVC target is unchanged.

$comdatvar = comdat any

@var = external thread_local global i32
@comdatvar = linkonce_odr thread_local global i32 1, comdat
@localvar = dso_local thread_local global i32 2

define i32 @getVar() {
; CHECK-LABEL: getVar:
; CHECK:         adrp x8, __imp_var$tls
; CHECK-NEXT:  [[REC:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    ldr x8, [x8, :lo12:__imp_var$tls]
; CHECK-NEXT:    ldr x10, [x18, #88]
; CHECK-NEXT:    .linkfusedaccess [[REC]], x8
; CHECK-NEXT:  [[ADDR:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    ldr x9, [x8]
; CHECK-NEXT:    .linkfusedaccess [[REC]], x8
; CHECK-NEXT:  [[OFF:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    ldr w8, [x8, #8]
; CHECK-NEXT:    .linkfusedaccess [[ADDR]], x9
; CHECK-NEXT:    ldr w9, [x9]
; CHECK-NEXT:    ldr x9, [x10, x9, lsl #3]
; CHECK-NEXT:    .linkfusedindex [[OFF]], x8
; CHECK-NEXT:    ldr w0, [x9, x8]
; O0-LABEL:    getVar:
; O0:            adrp x9, __imp_var$tls
; O0-NEXT:       ldr x9, [x9, :lo12:__imp_var$tls]
; MSVC-LABEL:  getVar:
; MSVC:          adrp x8, _tls_index
; MSVC:          add x8, x8, :secrel_hi12:var
  %p = call ptr @llvm.threadlocal.address.p0(ptr @var)
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @getComdatVar() {
; CHECK-LABEL: getComdatVar:
; CHECK:         adrp x8, __imp_comdatvar$tls
; CHECK:         ldr x8, [x8, :lo12:__imp_comdatvar$tls]
  %p = call ptr @llvm.threadlocal.address.p0(ptr @comdatvar)
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @getLocalVar() {
; CHECK-LABEL: getLocalVar:
; CHECK:         adrp x8, _tls_index
; CHECK:         add x8, x8, :secrel_hi12:localvar
; CHECK-NEXT:    ldr w0, [x8, :secrel_lo12:localvar]
  %p = call ptr @llvm.threadlocal.address.p0(ptr @localvar)
  %v = load i32, ptr %p
  ret i32 %v
}

declare ptr @llvm.threadlocal.address.p0(ptr)
