// RUN: %clang_cc1 -std=c++23 -fexperimental-pointer-field-protection-tagged -verify %s

struct [[clang::trivially_relocatable(true)]] Owner {
  void *pointer;
  Owner(Owner&&);
  ~Owner();
};
struct Outer { Owner owner; };
static_assert(!__builtin_is_bitwise_relocatable(Owner));
static_assert(!__builtin_is_bitwise_relocatable(Outer));
static_assert(__builtin_is_bitwise_relocatable(int));

void transfer(Owner *dest, Owner *src) {
  __builtin_trivially_relocate(dest, src, 1); // expected-error {{must be relocatable}}
  __builtin_trivially_relocate(dest, src, 1, true); // expected-error {{must be relocatable}}
}
