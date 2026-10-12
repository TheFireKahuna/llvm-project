// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++20 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm -o - %s | FileCheck --check-prefix=OTHER %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm -o - %s | FileCheck --check-prefix=OTHER %s

// Every definition of a replaceable function, and of libc++'s link-time
// replaceable __libcpp_verbose_abort, tells the linker that it is one, so that
// a DLL cannot export one in place of the runtime's default. An asm label names
// a function that no other image calls by its ABI name, a declaration defines
// nothing, and a placement form is not replaceable.

typedef decltype(sizeof(0)) size_t;
namespace std {
enum class align_val_t : size_t {};
struct nothrow_t {};
} // namespace std
extern "C" void *malloc(size_t);
extern "C" void free(void *);

void *operator new(size_t n) { return malloc(n); }
[[gnu::weak]] void operator delete(void *p, size_t, std::align_val_t) noexcept {
  free(p);
}
void *operator new[](size_t n, const std::nothrow_t &) noexcept {
  return malloc(n);
}
void operator delete(void *p) noexcept __asm__("__llvm_libc_delete");
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept;
void *operator new(size_t, void *p, int) noexcept { return p; }
void use() { operator delete[](nullptr); }

namespace std {
inline namespace __1 {
[[noreturn]] void __libcpp_verbose_abort(const char *, ...) noexcept {
  __builtin_trap();
}
} // namespace __1
} // namespace std

// CHECK:     !llvm.linker.options = !{![[M0:[0-9]+]], ![[M1:[0-9]+]], ![[M2:[0-9]+]], ![[M3:[0-9]+]]}
// CHECK-DAG: ![[M0]] = !{!"/OVERRIDABLE:_Znwy"}
// CHECK-DAG: ![[M1]] = !{!"/OVERRIDABLE:_ZdlPvySt11align_val_t"}
// CHECK-DAG: ![[M2]] = !{!"/OVERRIDABLE:_ZnayRKSt9nothrow_t"}
// CHECK-DAG: ![[M3]] = !{!"/OVERRIDABLE:_ZNSt3__122__libcpp_verbose_abortEPKcz"}
// CHECK-NOT: /OVERRIDABLE:

// OTHER-NOT: /OVERRIDABLE:
