// RUN: %clang_cc1 -std=c++23 -triple x86_64-linux-gnu -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -std=c++23 -triple x86_64-windows-msvc -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -std=c++23 -triple x86_64-linux-gnu -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=OPT

struct [[clang::trivially_relocatable(true)]] Owner {
  int *pointer;
  Owner(Owner&&);
  ~Owner();
};

// A nontrivial owner retains its indirect argument ABI.
// IR-LABEL: define {{.*}}void @argument(ptr{{.*}})
extern "C" void argument(Owner owner) {}

// Relocation needs no move call, source clearing, or source destruction.
// IR-LABEL: define {{.*}}ptr @relocate(
// IR: call void @llvm.memcpy{{.*}}i64 8, i1 false)
// IR: ret ptr
// OPT-LABEL: define {{.*}}ptr @relocate(
// OPT-NOT: call
// OPT: load i64, ptr %src
// OPT: store i64 {{.*}}, ptr %dest
// OPT-NEXT: ret ptr %dest
extern "C" Owner *relocate(Owner *dest, Owner *src) {
  return __builtin_trivially_relocate(dest, src, 1, true);
}

// IR-LABEL: define {{.*}}ptr @relocate_array(
// IR: mul i64 {{.*}}, 8
// IR: call void @llvm.memcpy
extern "C" Owner *relocate_array(Owner *dest, Owner *src, __SIZE_TYPE__ count) {
  return __builtin_trivially_relocate(dest, src, count, true);
}

// A single pointer-sized transfer needs the same load/store without a promise.
// IR-LABEL: define {{.*}}ptr @relocate_single_inferred(
// IR: call void @llvm.memmove
// OPT-LABEL: define {{.*}}ptr @relocate_single_inferred(
// OPT-NOT: call
// OPT: load i64, ptr %src
// OPT: store i64 {{.*}}, ptr %dest
// OPT-NEXT: ret ptr %dest
extern "C" Owner *relocate_single_inferred(Owner *dest, Owner *src) {
  return __builtin_trivially_relocate(dest, src, 1);
}

// Ordinary alias analysis supplies the same disjointness as an explicit flag.
// IR-LABEL: define {{.*}}ptr @relocate_array_inferred(
// IR: call void @llvm.memmove
// OPT-LABEL: define {{.*}}ptr @relocate_array_inferred(
// OPT-NOT: @llvm.memmove
// OPT: call void @llvm.memcpy
// OPT-NEXT: ret ptr %dest
extern "C" Owner *relocate_array_inferred(Owner *__restrict dest, Owner *__restrict src, __SIZE_TYPE__ count) {
  return __builtin_trivially_relocate(dest, src, count);
}

extern void consume_and_destroy(Owner *, __SIZE_TYPE__);

// Fresh, unescaped local storage is disjoint even without restrict annotations.
// IR-LABEL: define {{.*}}void @relocate_to_local(
// IR: call void @llvm.memmove
// OPT-LABEL: define {{.*}}void @relocate_to_local(
// OPT-NOT: @llvm.memmove
// OPT: call void @llvm.memcpy
extern "C" void relocate_to_local(Owner *src, __SIZE_TYPE__ count) {
  if (count > 4)
    return;
  alignas(Owner) unsigned char storage[4 * sizeof(Owner)];
  Owner *dest = __builtin_trivially_relocate((Owner*)storage, src, count);
  consume_and_destroy(dest, count);
}

// The original form remains overlap-safe, including for newly eligible owners.
// IR-LABEL: define {{.*}}ptr @relocate_overlap(
// IR: call void @llvm.memmove
// OPT-LABEL: define {{.*}}ptr @relocate_overlap(
// OPT-NOT: @llvm.memcpy
// OPT: call void @llvm.memmove
extern "C" Owner *relocate_overlap(Owner *dest, Owner *src, __SIZE_TYPE__ count) {
  return __builtin_trivially_relocate(dest, src, count);
}

// IR-LABEL: define {{.*}}ptr @relocate_overlap_explicit(
// IR: call void @llvm.memmove
// OPT-LABEL: define {{.*}}ptr @relocate_overlap_explicit(
// OPT-NOT: @llvm.memcpy
// OPT: call void @llvm.memmove
extern "C" Owner *relocate_overlap_explicit(Owner *dest, Owner *src, __SIZE_TYPE__ count) {
  return __builtin_trivially_relocate(dest, src, count, false);
}

// A disjoint request also works for types already accepted by the old builtin.
// IR-LABEL: define {{.*}}ptr @relocate_ints(
// IR: call void @llvm.memcpy
extern "C" int *relocate_ints(int *dest, int *src, __SIZE_TYPE__ count) {
  return __builtin_trivially_relocate(dest, src, count, true);
}

// OPT-LABEL: define {{.*}}ptr @zero(
// OPT-NOT: load
// OPT-NOT: store
// OPT-NOT: call
// OPT: ret ptr null
extern "C" Owner *zero() {
  return __builtin_trivially_relocate((Owner*)nullptr, (Owner*)nullptr, 0, true);
}
