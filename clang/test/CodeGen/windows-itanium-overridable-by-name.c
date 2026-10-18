// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -disable-llvm-passes -o - %s | FileCheck --check-prefixes=CHECK,X64 %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -mdefault-visibility-export-mapping=explicit -emit-llvm -disable-llvm-passes -o - %s | FileCheck --check-prefixes=CHECK,X64 %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -disable-llvm-passes -o - %s | FileCheck --check-prefixes=CHECK,A64 %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -disable-llvm-passes -o - %s | FileCheck --check-prefix=OTHER %s

// A weak function definition that the image exports is an overridable default
// of every image, as a shared object's weak definition is on ELF: another
// image's definition of the function replaces it by name. It begins by reading
// its home bucket in the block of the table that the running executable
// carries in its header page, found through the PEB; when the bucket is taken,
// the block's search function looks its key up and returns its replacement's
// RVA, to which it transfers with its arguments unchanged. Its key is the
// xxh3_64 hash of its name with bit 0 clear, and its home bucket the hash's top
// six bits. It tells the linker that it is one, and its address is the
// program's one replacement. A definition that the image does not export, or a
// strong one, is not a default.

// CHECK-LABEL: define weak {{.*}}i32 @hook(i32 noundef %x)
// X64:         [[PEB:%.*]] = load ptr, ptr addrspace(256) inttoptr (i64 96 to ptr addrspace(256)), align 8, !invariant.load ![[EMPTY:[0-9]+]]
// A64:         [[PEB:%.*]] = load ptr, ptr {{%.*}}, align 8, !invariant.load ![[EMPTY:[0-9]+]]
// CHECK-NEXT:  [[BASEP:%.*]] = getelementptr inbounds i8, ptr [[PEB]], i64 16
// CHECK-NEXT:  [[BASE:%.*]] = load ptr, ptr [[BASEP]], align 8, !invariant.load ![[EMPTY]]
// CHECK-NEXT:  [[HOMEP:%.*]] = getelementptr inbounds i8, ptr [[BASE]], i64 2784
// CHECK-NEXT:  [[HOME:%.*]] = load i32, ptr [[HOMEP]], align 8, !invariant.load ![[EMPTY]]
// CHECK-NEXT:  [[TAKEN:%.*]] = icmp ne i32 [[HOME]], 0
// CHECK-NEXT:  br i1 [[TAKEN]], label %overridable.replaced, label %overridable.default, !prof
// CHECK:     overridable.replaced:
// CHECK-NEXT:  [[RVA:%.*]] = call i32 @__llvm_override_find(ptr [[BASE]], i64 -5004002893962006062, i32 46)
// CHECK-NEXT:  [[FOUND:%.*]] = icmp ne i32 [[RVA]], 0
// CHECK-NEXT:  br i1 [[FOUND]], label %overridable.forward, label %overridable.default
// CHECK:     overridable.forward:
// CHECK-NEXT:  [[OFF:%.*]] = zext i32 [[RVA]] to i64
// CHECK-NEXT:  [[TARGET:%.*]] = getelementptr inbounds i8, ptr [[BASE]], i64 [[OFF]]
// CHECK-NEXT:  [[RET:%.*]] = musttail call i32 [[TARGET]](i32 noundef %x)
// CHECK-NEXT:  ret i32 [[RET]]
// CHECK:     overridable.default:
__attribute__((visibility("default"), weak)) int hook(int x) { return x + 1; }

// The search function, one in each object that needs it.
// CHECK:       define linkonce_odr hidden i32 @__llvm_override_find(ptr %0, i64 %1, i32 %2) [[FIND:#[0-9]+]] comdat

// CHECK-LABEL: define weak {{.*}}dllexport {{.*}}void @hook2()
// CHECK:         call i32 @__llvm_override_find(ptr {{%.*}}, i64 {{-?[0-9]+}}, i32 {{[0-9]+}})
__attribute__((dllexport, weak)) void hook2(void) {}

// CHECK-LABEL: define weak {{.*}}void @local_weak()
// CHECK-NOT:     __llvm_override_find
// CHECK:         ret void
__attribute__((visibility("hidden"), weak)) void local_weak(void) {}

// CHECK-LABEL: define {{.*}}void @strong()
// CHECK-NOT:     __llvm_override_find
// CHECK:         ret void
__attribute__((visibility("default"))) void strong(void) {}

// CHECK:       attributes #{{[0-9]+}} = { {{.*}}"overridable-default"
// CHECK:       attributes [[FIND]] = { cold {{.*}}memory(read)
// CHECK:       !llvm.linker.options = !{![[HOOK:[0-9]+]], ![[HOOK2:[0-9]+]]{{(, ![0-9]+)*}}}
// CHECK:       ![[HOOK]] = !{!"/OVERRIDABLEBYNAME:hook"}
// CHECK:       ![[HOOK2]] = !{!"/OVERRIDABLEBYNAME:hook2"}

// OTHER-NOT: __llvm_override_find
// OTHER-NOT: OVERRIDABLEBYNAME
