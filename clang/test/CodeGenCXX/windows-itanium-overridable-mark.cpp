// On Windows Itanium and NT-POSIX a definition of a function that a program
// replaces for every image says so with !overridable, which the object's link
// records state as a link fact: each replaceable global allocation function,
// std::__libcpp_verbose_abort and each function with overridable_default,
// which also states its slot in the override table and its forwarding. An
// asm label names a function that no other image calls by the name, and an
// allocation function that takes a hint is not replaceable.

// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++20 \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-ntposix -std=c++20 \
// RUN:   -fsized-deallocation -emit-obj -o %t.o %s
// RUN: llvm-readobj --coff-link-records %t.o | FileCheck %s --check-prefix=OBJ
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefix=ELF
// ELF-NOT: !overridable

typedef __SIZE_TYPE__ size_t;
namespace std {
enum class align_val_t : size_t {};
struct nothrow_t {};
inline namespace __1 {
void __libcpp_verbose_abort(const char *, ...) noexcept;
}
} // namespace std
enum class __hot_cold_t : unsigned char {};

// CHECK-DAG: define {{.*}}ptr @_Znwy(i64 {{.*}}) {{.*}}!overridable [[NONE:![0-9]+]]
// CHECK-DAG: [[NONE]] = !{}
void *operator new(size_t) { return nullptr; }
// CHECK-DAG: define {{.*}}void @_ZdaPvySt11align_val_t({{.*}}!overridable [[NONE]]
void operator delete[](void *, size_t, std::align_val_t) noexcept {}
// CHECK-DAG: define {{.*}}@_ZnwyRKSt9nothrow_t({{.*}}!overridable [[NONE]]
void *operator new(size_t, const std::nothrow_t &) noexcept { return nullptr; }
// CHECK-DAG: define {{.*}}@_ZNSt3__122__libcpp_verbose_abortEPKcz({{.*}}!overridable [[NONE]]
void std::__libcpp_verbose_abort(const char *, ...) noexcept {}

// CHECK-DAG: define weak {{.*}}@_ZdlPv({{.*}}!overridable [[SLOT:![0-9]+]]
// CHECK-DAG: [[SLOT]] = !{i32 2}
[[gnu::weak, clang::overridable_default(2)]] void operator delete(void *) noexcept {}
// CHECK-DAG: define weak {{.*}}@_ZdlPvy({{.*}}!overridable [[FORWARD:![0-9]+]]
// CHECK-DAG: [[FORWARD]] = !{i32 3, i32 2}
void operator delete(void *, size_t) noexcept __attribute__((overridable_default(3, 2)));
__attribute__((weak)) void operator delete(void *, size_t) noexcept {}
// CHECK-DAG: define {{.*}}@user({{.*}}!overridable [[DROPS:![0-9]+]]
// CHECK-DAG: [[DROPS]] = !{i32 31, i32 30, i32 1}
extern "C" [[clang::overridable_default(31, 30, true)]] void user() {}
// A middle argument dropped moves those after it.
// CHECK-DAG: define {{.*}}@user3({{.*}}!overridable [[MIDDLE:![0-9]+]]
// CHECK-DAG: [[MIDDLE]] = !{i32 30, i32 29, i32 2}
extern "C" [[clang::overridable_default(30, 29, true)]] void user3(void *, int, int) {}

// CHECK-DAG: define {{.*}}@_ZnwySt11align_val_t12__hot_cold_t({{[^!]*( !kcfi_type ![0-9]+)? {$}}
void *operator new(size_t, std::align_val_t, __hot_cold_t) { return nullptr; }
// CHECK-DAG: define {{.*}}@labelled({{[^!]*( !kcfi_type ![0-9]+)? {$}}
void *operator new[](size_t) __asm__("labelled");
void *operator new[](size_t) { return nullptr; }
// CHECK-DAG: define {{.*}}@_Z5plainv({{[^!]*( !kcfi_type ![0-9]+)? {$}}
void plain() {}

// OBJ:      Tag: Overridable (0x9)
// OBJ-NEXT: Operands (
// OBJ-NEXT: )
// OBJ:        0000: 0302
