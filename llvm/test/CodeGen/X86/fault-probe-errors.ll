; RUN: not llc -mtriple=x86_64-windows-ntposix < %s -o /dev/null 2>&1 | FileCheck %s

; CHECK: error: {{.*}}in function nopers{{.*}}: a fault probe needs an Itanium-style personality function
define i32 @nopers(ptr addrspace(280) %p) {
  %v = load volatile i32, ptr addrspace(280) %p
  ret i32 %v
}

declare i32 @__gxx_personality_seh0(...)

; CHECK: error: {{.*}}in function wide{{.*}}: only a 32-bit load can be a fault probe
define i64 @wide(ptr addrspace(280) %p) personality ptr @__gxx_personality_seh0 {
  %v = load volatile i64, ptr addrspace(280) %p
  ret i64 %v
}

; CHECK: error: {{.*}}in function store{{.*}}: only a 32-bit load can be a fault probe
define void @store(ptr addrspace(280) %p) personality ptr @__gxx_personality_seh0 {
  store volatile i32 0, ptr addrspace(280) %p
  ret void
}
