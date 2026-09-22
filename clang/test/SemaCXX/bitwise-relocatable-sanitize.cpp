// RUN: %clang_cc1 -triple x86_64-linux-gnu -std=c++23 -verify=plain %s
// RUN: %clang_cc1 -triple x86_64-linux-gnu -std=c++23 -fsanitize=address -fsanitize-address-field-padding=1 -DASAN -verify=asan %s
// RUN: %clang_cc1 -triple x86_64-linux-gnu -std=c++23 -fsanitize=address -fsanitize-address-field-padding=2 -DASAN -verify=asan %s
// plain-no-diagnostics

// Mixed access makes this non-standard-layout class eligible for ASan field
// padding. An explicit relocation permission must not bypass poisoned padding.
struct [[clang::trivially_relocatable(true)]] Padded {
  int first;
  Padded(Padded&&);
  ~Padded();
private:
  int second;
};

struct Derived : Padded {};
struct Nested { Padded value; };

#ifdef ASAN
static_assert(!__is_bitwise_cloneable(Padded));
static_assert(!__builtin_is_bitwise_relocatable(Padded));
static_assert(!__builtin_is_bitwise_relocatable(Padded[2]));
static_assert(!__builtin_is_bitwise_relocatable(Derived));
static_assert(!__builtin_is_bitwise_relocatable(Nested));
#else
static_assert(__builtin_is_bitwise_relocatable(Padded));
static_assert(__builtin_is_bitwise_relocatable(Padded[2]));
static_assert(__builtin_is_bitwise_relocatable(Derived));
static_assert(__builtin_is_bitwise_relocatable(Nested));
#endif

void transfer(Padded *dest, Padded *src) {
  __builtin_trivially_relocate(dest, src, 1); // asan-error {{must be relocatable}}
  __builtin_trivially_relocate(dest, src, 1, true); // asan-error {{must be relocatable}}
}
