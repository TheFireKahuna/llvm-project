// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -fsanitize=kcfi -emit-llvm -disable-llvm-passes -o - %s | FileCheck --check-prefixes=CHECK,X64 %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++20 -emit-llvm -disable-llvm-passes -o - %s | FileCheck --check-prefixes=CHECK,X64 %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++20 -emit-llvm -disable-llvm-passes -o - %s | FileCheck --check-prefixes=CHECK,A64 %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm -disable-llvm-passes -Wno-unknown-attributes -o - %s | FileCheck --check-prefix=OTHER %s

// A runtime's default of an overridable function begins by reading its slot
// in the table that the running executable carries in its header page, found
// through the PEB, and transfers there with its arguments unchanged when the
// slot holds its tag and the table its magic. The loads are invariant, so that
// the optimizer may share them, and the transfer carries no kcfi bundle.

typedef decltype(sizeof(0)) size_t;
namespace std { enum class align_val_t : size_t {}; }
extern "C" void *malloc(size_t);
extern "C" void free(void *);

// CHECK-LABEL: define weak {{.*}}ptr @_Zn{{w|m}}{{y|m}}(i64 noundef %n)
// X64:         [[PEB:%.*]] = load ptr, ptr addrspace(256) inttoptr (i64 96 to ptr addrspace(256)), align 8, !invariant.load ![[EMPTY:[0-9]+]]
// A64:         [[TEB:%.*]] = call i64 @llvm.read_register.i64(metadata ![[X18:[0-9]+]])
// A64-NEXT:    [[TEBP:%.*]] = inttoptr i64 [[TEB]] to ptr
// A64-NEXT:    [[PEBP:%.*]] = getelementptr inbounds i8, ptr [[TEBP]], i64 96
// A64-NEXT:    [[PEB:%.*]] = load ptr, ptr [[PEBP]], align 8, !invariant.load ![[EMPTY:[0-9]+]]
// CHECK-NEXT:  [[BASEP:%.*]] = getelementptr inbounds i8, ptr [[PEB]], i64 16
// CHECK-NEXT:  [[BASE:%.*]] = load ptr, ptr [[BASEP]], align 8, !invariant.load ![[EMPTY]]
// CHECK-NEXT:  [[SLOT:%.*]] = getelementptr inbounds i8, ptr [[BASE]], i64 64
// CHECK-NEXT:  [[TAG:%.*]] = load i32, ptr [[SLOT]], align 8, !invariant.load ![[EMPTY]]
// CHECK-NEXT:  [[HIT:%.*]] = icmp eq i32 [[TAG]], -975067149
// CHECK-NEXT:  br i1 [[HIT]], label %overridable.replaced, label %overridable.default, !prof ![[UNLIKELY:[0-9]+]]
// CHECK:     overridable.replaced:
// CHECK-NEXT:  [[MAGICP:%.*]] = getelementptr inbounds i8, ptr [[BASE]], i64 128
// CHECK-NEXT:  [[MAGIC:%.*]] = load i32, ptr [[MAGICP]], align 8, !invariant.load ![[EMPTY]]
// CHECK-NEXT:  [[TABLE:%.*]] = icmp eq i32 [[MAGIC]], 2067448533
// CHECK-NEXT:  br i1 [[TABLE]], label %overridable.forward, label %overridable.default
// CHECK:     overridable.forward:
// CHECK-NEXT:  [[RVAP:%.*]] = getelementptr inbounds i8, ptr [[BASE]], i64 68
// CHECK-NEXT:  [[RVA:%.*]] = load i32, ptr [[RVAP]], align 4, !invariant.load ![[EMPTY]]
// CHECK-NEXT:  [[OFF:%.*]] = zext i32 [[RVA]] to i64
// CHECK-NEXT:  [[TARGET:%.*]] = getelementptr inbounds i8, ptr [[BASE]], i64 [[OFF]]
// CHECK-NEXT:  [[RET:%.*]] = musttail call noalias noundef nonnull ptr [[TARGET]](i64 noundef %n) #{{[0-9]+}}{{$}}
// CHECK-NEXT:  ret ptr [[RET]]
// CHECK:     overridable.default:
// CHECK-NEXT:  store i64 %n
// CHECK:       call ptr @malloc
[[gnu::weak, clang::overridable_default(0)]] void *operator new(size_t n) {
  return malloc(n);
}

// A slot after the table's header is past the end of a table with fewer
// slots.
// CHECK-LABEL: define weak {{.*}}void @_ZdlPv{{y|m}}St11align_val_t(
// CHECK:         getelementptr inbounds i8, ptr {{%.*}}, i64 160
// CHECK:         icmp eq i32 {{%.*}}, -975067142
// CHECK:       overridable.replaced:
// CHECK:         icmp eq i32 {{%.*}}, 2067448533
// CHECK-NEXT:    [[COUNTP:%.*]] = getelementptr inbounds i8, ptr {{%.*}}, i64 134
// CHECK-NEXT:    [[COUNT:%.*]] = load i16, ptr [[COUNTP]], align 2, !invariant.load
// CHECK-NEXT:    [[IN:%.*]] = icmp ugt i16 [[COUNT]], 9
// CHECK-NEXT:    and i1 {{%.*}}, [[IN]]
// CHECK:       overridable.forward:
// CHECK:         getelementptr inbounds i8, ptr {{%.*}}, i64 164
// CHECK:         musttail call void {{%.*}}(ptr noundef %p, i64 noundef %0, i64 noundef %1) #{{[0-9]+}}{{$}}
// CHECK-NEXT:    ret void
[[gnu::weak, clang::overridable_default(9)]] void
operator delete(void *p, size_t, std::align_val_t) noexcept {
  free(p);
}

// A variadic default forwards its variable arguments.
// CHECK-LABEL: define weak {{.*}}void @_ZNSt3__122__libcpp_verbose_abortEPKcz(ptr noundef %0, ...)
// CHECK:       overridable.forward:
// CHECK:         musttail call void (ptr, ...) {{%.*}}(ptr noundef %0, ...) #{{[0-9]+}}{{$}}
// CHECK-NEXT:    ret void
namespace std {
inline namespace __1 {
[[gnu::weak, clang::overridable_default(20)]] void
__libcpp_verbose_abort(const char *, ...) noexcept {
  __builtin_trap();
}
} // namespace __1
} // namespace std

// OTHER-NOT: overridable.replaced

// CHECK-DAG: ![[EMPTY]] = !{}
// CHECK-DAG: ![[UNLIKELY]] = !{!"branch_weights", i32 1, i32 1048575}
// A64-DAG:   ![[X18]] = !{!"x18"}
