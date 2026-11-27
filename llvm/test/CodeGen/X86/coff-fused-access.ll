; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED --implicit-check-not=site_fact
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED --implicit-check-not=site_fact
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,PLAIN --implicit-check-not=FUSED \
; RUN:     --implicit-check-not=site_fact
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=O0 --implicit-check-not=FUSED \
; RUN:     --implicit-check-not=site_fact
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=A64,A64F --implicit-check-not=site_fact %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=A64,A64P --implicit-check-not=IF_LOCAL \
; RUN:     --implicit-check-not=site_fact %}

; On Windows Itanium and NT-POSIX an optimized function marks each load or
; store whose base, with no index, only the load of an import pointer reaches,
; naming the pointer, for the linker to make direct. A load whose register only
; such sites read is a fused read.

%struct = type { i32, i64 }
@v = external dllimport global i32
@s = external dllimport global %struct
@arr = external dllimport global [8 x i32]
declare void @taking(ptr)

; Each access of several is one.
define void @write(i32 %a, i64 %b) {
; CHECK-LABEL: write:
; FUSED:         movq __imp_s@FUSED(%rip), %rax
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_s
; PLAIN:         movq __imp_s(%rip), %rax
; CHECK-NEXT:    movl %e{{[a-z]+}}, (%rax)
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_s
; CHECK-NEXT:    movq %r{{[a-z]+}}, 8(%rax)
; O0-LABEL: write:
; O0:            movq __imp_s(%rip), %rax
; A64-LABEL: write:
; A64F:          ldr x8, [x8, :fused_lo12:__imp_s]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_s
; A64P:          ldr x8, [x8, :lo12:__imp_s]
; A64-NEXT:      str w0, [x8]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_s
; A64-NEXT:      str x1, [x8, #8]
  store i32 %a, ptr @s
  %f = getelementptr %struct, ptr @s, i32 0, i32 1
  store i64 %b, ptr %f
  ret void
}

; An access with an index is none, and its load stays one.
define i32 @indexed(i64 %i) {
; CHECK-LABEL: indexed:
; CHECK:         movq __imp_arr(%rip), %rax
; CHECK-NEXT:    movl (%rax,%r{{[a-z]+}},4), %eax
; A64-LABEL: indexed:
; A64:           ldr x8, [x8, :lo12:__imp_arr]
; A64-NEXT:      ldr w0, [x8, x0, lsl #2]
  %p = getelementptr [8 x i32], ptr @arr, i64 0, i64 %i
  %x = load i32, ptr %p
  ret i32 %x
}

; Where another reader keeps the load, x86-64 states an access in the load's
; block, which needs no register once the linker has made it direct. On
; AArch64 an access takes a page offset only with its load dropped.
define i32 @kept() {
; CHECK-LABEL: kept:
; CHECK:         movq __imp_v(%rip), [[REG:%r[a-z]+]]
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
; CHECK-NEXT:    movl ([[REG]]), %e{{[a-z]+}}
; CHECK-NEXT:    callq taking
; A64-LABEL: kept:
; A64:           ldr x0, [x0, :lo12:__imp_v]
; A64-NEXT:      ldr w19, [x0]
  %x = load i32, ptr @v
  call void @taking(ptr @v)
  ret i32 %x
}

; A pointer the compiler chose is loaded again behind a call when read on both
; sides, as the linker may remove each load, where a callee-saved register
; would stay. A dllimport variable's pointer, whose load stays, is held; AArch64
; holds both, as other targets do.
@chosen = external global [8 x i32]

define i32 @across() {
; CHECK-LABEL: across:
; FUSED:         movq __imp_chosen@FUSED(%rip), %rax
; FUSED:         movq __imp_taking(%rip), %r{{cx|di}}
; FUSED-NEXT:    callq taking
; FUSED-NEXT:    movq __imp_arr@FUSED(%rip), %r[[HELD:[a-z0-9]+]]
; FUSED:         movq __imp_chosen@FUSED(%rip), %rax
; FUSED:         movq __imp_taking(%rip), %r{{cx|di}}
; FUSED-NEXT:    callq taking
; FUSED:         addl 4(%r[[HELD]]), %e
; PLAIN:         movq __imp_arr(%rip), %r[[HELD:[a-z0-9]+]]
; PLAIN:         callq taking
; PLAIN:         addl 4(%r[[HELD]]), %e
; A64-LABEL: across:
; A64F:          ldr x[[CHOSEN:19|2[0-9]]], [x[[CHOSEN]], :fused_lo12:__imp_chosen]
; A64F:          bl taking
; A64F:          ldr w{{[0-9]+}}, [x[[CHOSEN]], #4]
  %a = load i32, ptr @chosen
  call void @taking(ptr @taking)
  %b = load i32, ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 1)
  %c = load i32, ptr @arr
  call void @taking(ptr @taking)
  %d = load i32, ptr getelementptr ([8 x i32], ptr @arr, i64 0, i64 1)
  %ab = add i32 %a, %b
  %cd = add i32 %c, %d
  %sum = add i32 %ab, %cd
  ret i32 %sum
}

; The address of a function is an instruction wherever it is loaded again,
; so one that is read in a loop that contains a call is held, as the sum of
; a pointer and a constant is. A copy of it is no such read: the load takes
; the copy's place.
define void @repeated(ptr %o, i32 %n) {
; CHECK-LABEL: repeated:
; FUSED:         movq __imp_taking(%rip), %r[[HELD:bx|si|di|bp|1[2-5]]]
; FUSED:       .LBB{{.*}}:
; FUSED:         movq %r[[HELD]], (%r
; FUSED-NEXT:    movq __imp_get(%rip), %r{{cx|di}}
; FUSED-NEXT:    callq taking
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  store ptr @taking, ptr %o
  call void @taking(ptr @get)
  %next = add i32 %i, 1
  %more = icmp ne i32 %next, %n
  br i1 %more, label %body, label %done
done:
  ret void
}

; So is the pointer of a variable where it is read as an address: only a
; load or a store through it takes the load of the pointer into itself.
define void @address(ptr %o, i32 %n) {
; CHECK-LABEL: address:
; FUSED:         movq __imp_chosen(%rip), %r[[HELD:bx|si|di|bp|1[2-5]]]
; FUSED:       .LBB{{.*}}:
; FUSED:         movq %r[[HELD]], (%r
; FUSED-NOT:     __imp_chosen
; FUSED:         callq taking
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  store ptr @chosen, ptr %o
  call void @taking(ptr %o)
  %next = add i32 %i, 1
  %more = icmp ne i32 %next, %n
  br i1 %more, label %body, label %done
done:
  ret void
}

; Splitting loads the pointer again where a part of its range begins, so a
; part has several loads of the one pointer: it is that pointer still, and
; no callee-saved register holds it around the calls of a loop.
define i32 @split(ptr %o, i32 %n) {
; CHECK-LABEL: split:
; FUSED-NOT:     __imp_chosen
; FUSED:         callq taking
; FUSED-NEXT:    movq __imp_chosen@FUSED(%rip), %rax
; FUSED:         addl 8(%rax), %e
; FUSED:         callq get
; FUSED-NEXT:    movq __imp_chosen{{.*}}(%rip), %r[[AGAIN:[a-z0-9]+]]
; FUSED:         movl (%r[[AGAIN]]), %e
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc2, %latch ]
  %g = call i32 @get(ptr %o)
  %a = load i32, ptr @chosen
  %ag = add i32 %a, %g
  %odd = icmp eq i32 %g, 0
  br i1 %odd, label %left, label %right
left:
  call void @taking(ptr %o)
  br label %latch
right:
  call void @taking(ptr null)
  br label %latch
latch:
  %b = load i32, ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2)
  %v = add i32 %ag, %b
  %acc2 = add i32 %acc, %v
  %next = add i32 %i, 1
  %more = icmp ne i32 %next, %n
  br i1 %more, label %body, label %done
done:
  ret i32 %acc2
}
declare i32 @get(ptr)

; A read in a block that the pointer's load does not precede counts as one
; behind a call, though no call reaches it: only a thread-local value's reads
; count where a call reaches them. Read in three such blocks on one path, across
; asm that takes each register a call may change, the pointer is held in a
; callee-saved register and loaded once.
define i32 @held(i64 %i, i64 %j, i64 %k, i1 %c, i1 %d) {
; CHECK-LABEL: held:
; FUSED:         movq __imp_chosen(%rip), %r[[HELD:bx|si|di|bp|1[2-5]]]
; FUSED-NOT:     __imp_chosen
; FUSED-COUNT-4: (%r[[HELD]],%r{{[a-z0-9]+}},4)
; FUSED-NOT:     __imp_chosen
; FUSED:         retq
entry:
  %p = getelementptr [8 x i32], ptr @chosen, i64 0, i64 %i
  %a = load i32, ptr %p
  call void asm sideeffect "", "~{rax},~{rcx},~{rdx},~{r8},~{r9},~{r10},~{r11}"()
  br i1 %c, label %one, label %done
one:
  %q = getelementptr [8 x i32], ptr @chosen, i64 0, i64 %j
  %b = load i32, ptr %q
  %ab = add i32 %a, %b
  call void asm sideeffect "", "~{rax},~{rcx},~{rdx},~{r8},~{r9},~{r10},~{r11}"()
  br i1 %d, label %two, label %done
two:
  %r = getelementptr [8 x i32], ptr @chosen, i64 0, i64 %k
  %e = load i32, ptr %r
  %abe = add i32 %ab, %e
  call void asm sideeffect "", "~{rax},~{rcx},~{rdx},~{r8},~{r9},~{r10},~{r11}"()
  br i1 %c, label %three, label %done
three:
  %t = getelementptr [8 x i32], ptr @chosen, i64 0, i64 %j
  %f = load i32, ptr %t
  %abef = add i32 %abe, %f
  br label %done
done:
  %s = phi i32 [ %a, %entry ], [ %ab, %one ], [ %abe, %two ], [ %abef, %three ]
  ret i32 %s
}

; A constant that every image defines alike reads the same in each, so the
; object states where the image's own definition serves: 7 at a load of the
; import pointer whose register only reads of memory read, as the base of
; their address, at the adrp that gives it the pointer's page and at each
; fused site of such a load. The address of
; another image's definition is still the one that anything else gets: a
; constant that may change, one that another definition may replace, and a
; volatile read state nothing.
$k = comdat any
$tbl = comdat any
$mut = comdat any
@k = linkonce_odr constant i32 7, comdat
@tbl = weak_odr constant [8 x i32] zeroinitializer, comdat
@mut = linkonce_odr global i32 7, comdat
@any = weak constant i32 7
@inl = available_externally constant i32 7
declare void @lent(ptr captures(none))

define i32 @value() {
; FUSED-LABEL: value:
; FUSED:         .site_fact 7, 0
; FUSED-NEXT:    movq __imp_k@FUSED(%rip), %rax
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_k
; FUSED-NEXT:    .site_fact 7, 0
; FUSED-NEXT:    movl (%rax), %eax
; A64F-LABEL: value:
; A64F:          .site_fact 7, 0
; A64F-NEXT:     adrp x8, __imp_k
; A64F-NEXT:     .site_fact 7, 0
; A64F-NEXT:     ldr x8, [x8, :fused_lo12:__imp_k]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_k
; A64F-NEXT:     .site_fact 7, 0
; A64F-NEXT:     ldr w0, [x8]
  %x = load i32, ptr @k
  ret i32 %x
}

define i32 @element(i64 %i) {
; FUSED-LABEL: element:
; FUSED:         .site_fact 7, 0
; FUSED-NEXT:    movq __imp_tbl(%rip), %rax
; FUSED-NEXT:    movl (%rax,%r{{[a-z]+}},4), %eax
; A64F-LABEL: element:
; A64F:          .site_fact 7, 0
; A64F-NEXT:     adrp x8, __imp_tbl
; A64F-NEXT:     .site_fact 7, 0
; A64F-NEXT:     ldr x8, [x8, :lo12:__imp_tbl]
; A64F-NEXT:     ldr w0, [x8, x0, lsl #2]
  %p = getelementptr [8 x i32], ptr @tbl, i64 0, i64 %i
  %x = load i32, ptr %p
  ret i32 %x
}

; An atomic read tells nothing of the address, so the function's loads of
; the pointer are stated together.
define i32 @unlike() {
; FUSED-LABEL: unlike:
; FUSED:         .site_fact 7, 0
; FUSED-NEXT:    movq __imp_inl@FUSED(%rip), %r{{[a-z0-9]+}}
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_inl
; FUSED-NEXT:    .site_fact 7, 0
; A64F-LABEL: unlike:
; A64F:          .site_fact 7, 0
; A64F-NEXT:     adrp x11, __imp_inl
; A64F-NEXT:     .site_fact 7, 0
; A64F-NEXT:     ldr x11, [x11, :lo12:__imp_inl]
  %a = load i32, ptr @mut
  %b = load i32, ptr @any
  %c = load volatile i32, ptr @k
  %d = load atomic i32, ptr @inl seq_cst, align 4
  %ab = add i32 %a, %b
  %cd = add i32 %c, %d
  %s = add i32 %ab, %cd
  ret i32 %s
}

; Where a load stays for an address that the function gives away, an access
; that only reads is bound alone, with 12.
define i32 @taken() {
; FUSED-LABEL: taken:
; FUSED:         movq __imp_k(%rip), %r[[REG:cx|di]]
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_k
; FUSED-NEXT:    .site_fact 12, 0
; FUSED-NEXT:    movl (%r[[REG]]), %e{{[a-z]+}}
; A64F-LABEL: taken:
; A64F:          ldr x0, [x0, :lo12:__imp_k]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_k
; A64F-NEXT:     .site_fact 12, 0
; A64F-NEXT:     ldr w19, [x0]
  %x = load i32, ptr @k
  call void @taking(ptr @k)
  ret i32 %x
}

; A function that only reads through the address, lends it to a callee that
; keeps nothing of it, and compares addresses in the constant with one
; another has every load of the pointer stated, whatever reads its register:
; the addresses that the loads lead to still compare as they did.
define i32 @walked() {
; FUSED-LABEL: walked:
; FUSED:         .site_fact 7, 0
; FUSED-NEXT:    movq __imp_tbl(%rip), %r{{cx|di}}
; FUSED-NEXT:    callq lent
; FUSED-NEXT:    .site_fact 7, 0
; FUSED-NEXT:    movq __imp_tbl(%rip), %r{{[a-z]+}}
; A64F-LABEL: walked:
; A64F:          .site_fact 7, 0
; A64F-NEXT:     adrp x19, __imp_tbl
; A64F-NEXT:     .site_fact 7, 0
; A64F-NEXT:     ldr x19, [x19, :lo12:__imp_tbl]
; A64F-NEXT:     mov x0, x19
; A64F-NEXT:     bl lent
entry:
  call void @lent(ptr @tbl)
  br label %loop
loop:
  %p = phi ptr [ @tbl, %entry ], [ %n, %loop ]
  %s = phi i32 [ 0, %entry ], [ %s2, %loop ]
  %v = load i32, ptr %p
  %s2 = add i32 %s, %v
  %n = getelementptr i32, ptr %p, i64 1
  %e = icmp eq ptr %n, getelementptr ([8 x i32], ptr @tbl, i64 0, i64 8)
  br i1 %e, label %done, label %loop
done:
  ret i32 %s2
}

; A comparison with any other address is of the one address.
define i1 @compared(ptr %p) {
; FUSED-LABEL: compared:
; FUSED:         movq __imp_inl(%rip), %rax
; A64F-LABEL: compared:
; A64F:          ldr x8, [x8, :lo12:__imp_inl]
  %e = icmp eq ptr %p, @inl
  ret i1 %e
}

; The allocator reads the uses of a chosen pointer before the frame is laid
; out, where the base of a store's address may be a stack slot: such a store
; is no access through the pointer.
define void @slot(i64 %i) {
; CHECK-LABEL: slot:
; FUSED:         movq __imp_chosen(%rip), %rax
; FUSED-NEXT:    movq %rax, {{[0-9]+}}(%rsp,%r{{cx|di}})
  %a = alloca [4 x ptr]
  %p = getelementptr i8, ptr %a, i64 %i
  store ptr @chosen, ptr %p
  call void @taking(ptr %a)
  ret void
}
