; RUN: not llc -mtriple=x86_64-pc-windows-ntposix -mcpu=x86-64-v3 < %s 2>&1 | FileCheck %s
; RUN: not llc -mtriple=aarch64-pc-windows-ntposix -mattr=-lse < %s 2>&1 | FileCheck %s --check-prefix=NOLSE

; A fault probe is one instruction in a function with an Itanium-style
; personality; anything else is diagnosed rather than left unrecoverable.

declare i32 @rust_eh_personality(...)

; CHECK: error: {{.*}}in function nand {{.*}}: a fault probe must lower to one instruction
; NOLSE: error: {{.*}}in function nand {{.*}}: a fault probe must lower to one instruction
define i64 @nand(ptr %p) personality ptr @rust_eh_personality {
  %q = addrspacecast ptr %p to ptr addrspace(280)
  %r = atomicrmw nand ptr addrspace(280) %q, i64 3 seq_cst, align 8
  ret i64 %r
}

; CHECK: error: {{.*}}in function wide_add {{.*}}: a fault probe must lower to one instruction
define i128 @wide_add(ptr %p, i128 %v) personality ptr @rust_eh_personality {
  %q = addrspacecast ptr %p to ptr addrspace(280)
  %r = atomicrmw add ptr addrspace(280) %q, i128 %v seq_cst, align 16
  ret i128 %r
}

; NOLSE: error: {{.*}}in function swap {{.*}}: a fault probe must lower to one instruction
define i64 @swap(ptr %p, i64 %v) personality ptr @rust_eh_personality {
  %q = addrspacecast ptr %p to ptr addrspace(280)
  %r = atomicrmw xchg ptr addrspace(280) %q, i64 %v seq_cst, align 8
  ret i64 %r
}

; CHECK: error: {{.*}}in function no_personality {{.*}}: a fault probe needs an Itanium-style personality function
define i64 @no_personality(ptr %p) {
  %q = addrspacecast ptr %p to ptr addrspace(280)
  %v = load atomic i64, ptr addrspace(280) %q acquire, align 8
  ret i64 %v
}
