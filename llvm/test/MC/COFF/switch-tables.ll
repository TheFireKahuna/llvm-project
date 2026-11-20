; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=X64
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -O0 < %s | FileCheck %s --check-prefix=X64-O0
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -relocation-model=static < %s | \
; RUN:   FileCheck %s --check-prefix=STATIC
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -aarch64-min-jump-table-entries=4 < %s | \
; RUN:   FileCheck %s --check-prefix=A64
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -aarch64-min-jump-table-entries=4 -O0 < %s | \
; RUN:   FileCheck %s --check-prefix=A64-O0
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s -o %t.o
; RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s --check-prefix=OBJ
; RUN: llvm-objdump -d --no-show-raw-insn --no-leading-addr %t.o | FileCheck %s --check-prefix=DIS
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -aarch64-min-jump-table-entries=4 -filetype=obj < %s -o %t.a.o
; RUN: llvm-readobj --coff-link-records %t.a.o | FileCheck %s --check-prefix=A64-OBJ
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -x86-speculative-load-hardening < %s | \
; RUN:   FileCheck %s --check-prefix=SLH
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -x86-speculative-load-hardening \
; RUN:   -x86-slh-post-load=false < %s | FileCheck %s --check-prefix=SLH-ADDR
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -mcpu=atom < %s | \
; RUN:   FileCheck %s --check-prefix=ATOM
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | \
; RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=switch_table --implicit-check-not=.Ltmp

; An object for Windows Itanium or NT-POSIX states each dispatch through a
; jump table: labels at the branch, at the table and at what the entries are
; relative to, the number of entries, their type in CodeView's numbers, a
; label at the instruction that loads the entry, and whether a check of the
; index precedes the dispatch. On x86-64 the load is the movslq, and it and
; the branch are as for any other triple. A table that several dispatches use
; is stated with each.

; X64-LABEL: checked:
; X64:         leaq .LJTI0_0(%rip), %rcx
; X64-NEXT:  [[LOAD:.Ltmp[0-9]+]]:
; X64-NEXT:    movslq (%rcx,%rax,4), %rax
; X64-NEXT:    addq %rcx, %rax
; X64-NEXT:  [[BRANCH:.Ltmp[0-9]+]]:
; X64-NEXT:    jmpq *%rax
; X64:       .LJTI0_0:
; X64-NEXT:    .long .LBB0_{{[0-9]+}}-.LJTI0_0
; X64:         .switch_table [[BRANCH]], .LJTI0_0, .LJTI0_0, 5, 4, [[LOAD]], 1
; X64-LABEL: interp:
; X64:       [[LOAD1:.Ltmp[0-9]+]]:
; X64-NEXT:    movslq (%rdi,%rax,4), %rax
; X64-NEXT:    addq %rdi, %rax
; X64-NEXT:  [[BRANCH1:.Ltmp[0-9]+]]:
; X64-NEXT:    jmpq *%rax
; X64:       [[LOAD2:.Ltmp[0-9]+]]:
; X64-NEXT:    movslq (%rdi,%rax,4), %rax
; X64-NEXT:    addq %rdi, %rax
; X64-NEXT:  [[BRANCH2:.Ltmp[0-9]+]]:
; X64-NEXT:    jmpq *%rax
; X64:         .switch_table [[BRANCH1]], .LJTI1_0, .LJTI1_0, 5, 4, [[LOAD1]], 0
; X64-NEXT:    .switch_table [[BRANCH2]], .LJTI1_0, .LJTI1_0, 5, 4, [[LOAD2]], 0

; NTPOSIX: .switch_table .Ltmp{{[0-9]+}}, .LJTI0_0, .LJTI0_0, 5, 4, .Ltmp{{[0-9]+}}, 1

; The registers differ without optimization, and the statement does not.
; X64-O0-LABEL: checked:
; X64-O0:       [[LOAD:.Ltmp[0-9]+]]:
; X64-O0-NEXT:    movslq (%rax,%rcx,4), %rcx
; X64-O0-NEXT:    addq %rcx, %rax
; X64-O0-NEXT:  [[BRANCH:.Ltmp[0-9]+]]:
; X64-O0-NEXT:    jmpq *%rax
; X64-O0:         .switch_table [[BRANCH]], .LJTI0_0, .LJTI0_0, 5, 4, [[LOAD]], 1
; X64-O0:         .switch_table {{.*}}, .LJTI1_0, .LJTI1_0, 5, 4, {{.*}}, 0

; The load is a load to every pass that hardens or tunes one. Speculative
; load hardening hardens what it loaded, as for any other triple, or its
; address, and the label stays at the instruction that reads the entry. A
; processor whose address generation stalls on a register just written gets
; the index from an LEA.
; SLH-LABEL: checked:
; SLH:         leaq .LJTI0_0(%rip), %rdx
; SLH-NEXT:  [[LOAD:.Ltmp[0-9]+]]:
; SLH-NEXT:    movslq (%rdx,%rcx,4), %rcx
; SLH-NEXT:    addq %rdx, %rcx
; SLH-NEXT:    orq %rax, %rcx
; SLH-NEXT:  [[BRANCH:.Ltmp[0-9]+]]:
; SLH-NEXT:    jmpq *%rcx
; SLH:         .switch_table [[BRANCH]], .LJTI0_0, .LJTI0_0, 5, 4, [[LOAD]], 1

; SLH-ADDR-LABEL: checked:
; SLH-ADDR:         leaq .LJTI0_0(%rip), %rdx
; SLH-ADDR-NEXT:    movq %rax, %r8
; SLH-ADDR-NEXT:    orq %rdx, %r8
; SLH-ADDR-NEXT:    orq %rax, %rcx
; SLH-ADDR-NEXT:  [[LOAD:.Ltmp[0-9]+]]:
; SLH-ADDR-NEXT:    movslq (%r8,%rcx,4), %rcx
; SLH-ADDR-NEXT:    addq %rdx, %rcx
; SLH-ADDR-NEXT:    movq %rax, %rdx
; SLH-ADDR-NEXT:    orq %rcx, %rdx
; SLH-ADDR-NEXT:  [[BRANCH:.Ltmp[0-9]+]]:
; SLH-ADDR-NEXT:    jmpq *%rdx
; SLH-ADDR:         .switch_table [[BRANCH]], .LJTI0_0, .LJTI0_0, 5, 4, [[LOAD]], 1

; ATOM-LABEL: checked:
; ATOM:         leal (%rcx), %eax
; ATOM-NEXT:    leaq .LJTI0_0(%rip), %rdx
; ATOM-NEXT:  [[LOAD:.Ltmp[0-9]+]]:
; ATOM-NEXT:    movslq (%rdx,%rax,4), %rax

; The branch through a table of addresses is also its load, and the table is
; its own base.
; STATIC-LABEL: checked:
; STATIC:       [[BRANCH:.Ltmp[0-9]+]]:
; STATIC-NEXT:    jmpq *.LJTI0_0(,%rax,8)
; STATIC:         .quad .LBB0_{{[0-9]+}}
; STATIC:         .switch_table [[BRANCH]], .LJTI0_0, .LJTI0_0, 5, 6, [[BRANCH]], 1

; On AArch64 the load is the first of the three instructions that form the
; target, and the base is the lowest target of a table of bytes, 7, or the
; first of the three for a table of words.
; A64-LABEL: checked:
; A64:       [[LOAD:.Ltmp[0-9]+]]:
; A64-NEXT:    adr x10, [[BASE:.LBB0_[0-9]+]]
; A64-NEXT:    ldrb w11, [x9, x8]
; A64-NEXT:    add x10, x10, x11, lsl #2
; A64-NEXT:  [[BRANCH:.Ltmp[0-9]+]]:
; A64-NEXT:    br x10
; A64:         .byte ([[BASE]]-[[BASE]])>>2
; A64:         .switch_table [[BRANCH]], .LJTI0_0, [[BASE]], 5, 7, [[LOAD]], 1
; A64:         .switch_table {{.*}}, .LJTI1_0, {{.*}}, 5, 7, {{.*}}, 0

; A64-O0-LABEL: checked:
; A64-O0:       [[LOAD:.Ltmp[0-9]+]]:
; A64-O0-NEXT:  [[BASE:.Ltmp[0-9]+]]:
; A64-O0-NEXT:    adr x8, [[BASE]]
; A64-O0-NEXT:    ldrsw x9, [x10, x11, lsl #2]
; A64-O0-NEXT:    add x8, x8, x9
; A64-O0-NEXT:  [[BRANCH:.Ltmp[0-9]+]]:
; A64-O0-NEXT:    br x8
; A64-O0:         .switch_table [[BRANCH]], .LJTI0_0, [[BASE]], 5, 4, [[LOAD]], 1

; The object names each place by a label, in the Sections group of its link
; records after the sections that the linker may resize, and each entry of
; the table names a label.
; DIS:      <$L{{[0-9a-f]+}}>:
; DIS-NEXT:   movslq (%rcx,%rax,4), %rax
; DIS-NEXT:   addq %rcx, %rax
; DIS-EMPTY:
; DIS-NEXT: <$L{{[0-9a-f]+}}>:
; DIS-NEXT:   jmpq *%rax
; OBJ:        Section ({{[0-9]+}}) .rdata {
; OBJ-NEXT:     0x0 IMAGE_REL_AMD64_REL32 $L{{[0-9a-f]+}}
; OBJ:          Kind: Sections (0x2)
; OBJ-NEXT:     ResizableSection: .text ({{[0-9]+}})
; OBJ-NEXT:     SwitchTable {
; OBJ-NEXT:       Branch: $L{{[0-9a-f]+}} ({{[0-9]+}})
; OBJ-NEXT:       Table: .LJTI0_0 ({{[0-9]+}})
; OBJ-NEXT:       Base: .LJTI0_0 ({{[0-9]+}})
; OBJ-NEXT:       Count: 5
; OBJ-NEXT:       Type: Int32 (0x4)
; OBJ-NEXT:       Load: $L{{[0-9a-f]+}} ({{[0-9]+}})
; OBJ-NEXT:       Flags: 1
; OBJ-NEXT:     }
; OBJ-NEXT:     SwitchTable {
; OBJ:            Table: .LJTI1_0 ({{[0-9]+}})
; OBJ:            Flags: 0
; OBJ:          SwitchTable {

; A64-OBJ:      Kind: Sections (0x2)
; A64-OBJ-NEXT: ResizableSection: .text ({{[0-9]+}})
; A64-OBJ-NEXT: SwitchTable {
; A64-OBJ:        Table: .LJTI0_0
; A64-OBJ:        Count: 5
; A64-OBJ-NEXT:   Type: UInt8ShiftLeft (0x7)
; A64-OBJ:        Flags: 1

; Another triple's output has no label for a dispatch and no statement.
; MSVC:      movslq (%rcx,%rax,4), %rax
; MSVC-NEXT: addq %rcx, %rax
; MSVC-NEXT: jmpq *%rax

declare void @a()
declare void @b()
declare void @c()
declare void @d()
declare void @e()

define void @checked(i32 %x) {
entry:
  switch i32 %x, label %def [ i32 0, label %l0
                              i32 1, label %l1
                              i32 2, label %l2
                              i32 3, label %l3
                              i32 4, label %l4 ]
l0:
  call void @a()
  ret void
l1:
  call void @b()
  ret void
l2:
  call void @c()
  ret void
l3:
  call void @d()
  ret void
l4:
  call void @e()
  ret void
def:
  ret void
}

define void @interp(ptr %p) {
entry:
  br label %disp
disp:
  %pc = phi ptr [ %p, %entry ], [ %n0, %l0 ], [ %n1, %l1 ], [ %n2, %l2 ], [ %n3, %l3 ]
  %op = load i64, ptr %pc
  switch i64 %op, label %def [ i64 0, label %l0
                               i64 1, label %l1
                               i64 2, label %l2
                               i64 3, label %l3
                               i64 4, label %l4 ]
l0:
  call void @a()
  %n0 = getelementptr i64, ptr %pc, i64 1
  br label %disp
l1:
  call void @b()
  %n1 = getelementptr i64, ptr %pc, i64 2
  br label %disp
l2:
  call void @c()
  %n2 = getelementptr i64, ptr %pc, i64 3
  br label %disp
l3:
  call void @d()
  %n3 = getelementptr i64, ptr %pc, i64 4
  br label %disp
l4:
  ret void
def:
  unreachable
}
