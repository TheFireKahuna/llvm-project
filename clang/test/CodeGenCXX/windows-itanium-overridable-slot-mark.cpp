// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++20 -emit-llvm -o - %s | FileCheck %s

// A runtime's default with a fixed slot records the slot in its mark, and, for
// a default specified as calling another, that other's slot, then 1 when that
// one takes this one's arguments without the second, so that the link of the
// runtime knows them and writes them into its import library. A function the
// standard does not list as replaceable is marked when it has a slot. The
// runtime's defaults are weak, so the module names no replacement marker.

typedef decltype(sizeof(0)) size_t;
namespace std {
enum class align_val_t : size_t {};
} // namespace std

[[gnu::weak, clang::overridable_default(0)]] void *operator new(size_t) {
  return nullptr;
}
[[gnu::weak, clang::overridable_default(1, 0)]] void *operator new[](size_t) {
  return nullptr;
}
[[gnu::weak, clang::overridable_default(9, 7, true)]] void
operator delete(void *, size_t, std::align_val_t) noexcept {}
namespace std {
[[gnu::weak, clang::overridable_default(21)]] bool is_debugger_present() noexcept {
  return false;
}
} // namespace std

// CHECK:     !llvm.linker.options = !{![[M0:[0-9]+]], ![[M1:[0-9]+]], ![[M2:[0-9]+]], ![[M3:[0-9]+]]}
// CHECK-DAG: ![[M0]] = !{!"/OVERRIDABLE:_Znwy,0"}
// CHECK-DAG: ![[M1]] = !{!"/OVERRIDABLE:_Znay,1,0"}
// CHECK-DAG: ![[M2]] = !{!"/OVERRIDABLE:_ZdlPvySt11align_val_t,9,7,1"}
// CHECK-DAG: ![[M3]] = !{!"/OVERRIDABLE:_ZSt19is_debugger_presentv,21"}
// CHECK-NOT: __llvm_overrides_v1
