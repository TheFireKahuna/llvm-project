// RUN: %clang_cc1 -std=c++23 -emit-pch -o %t %s
// RUN: %clang_cc1 -std=c++23 -include-pch %t -verify %s
// RUN: %clang_cc1 -std=c++23 -include %s -verify %s

#ifndef HEADER
#define HEADER
template<bool Enable>
struct [[clang::trivially_relocatable(Enable)]] Owner {
  int *pointer;
  Owner();
  Owner(Owner&&);
  ~Owner();
};
inline auto closure() { return [owner = Owner<true>{}] {}; }
template<bool Disjoint>
Owner<true> *transfer_owner(Owner<true> *dest, Owner<true> *src) {
  return __builtin_trivially_relocate(dest, src, 1, Disjoint);
}
#else
// expected-no-diagnostics
static_assert(__builtin_is_bitwise_relocatable(Owner<true>));
static_assert(!__builtin_is_bitwise_relocatable(Owner<false>));
static_assert(__builtin_is_bitwise_relocatable(decltype(closure())));
template Owner<true> *transfer_owner<true>(Owner<true>*, Owner<true>*);
template Owner<true> *transfer_owner<false>(Owner<true>*, Owner<true>*);
void transfer(decltype(closure()) *dest, decltype(closure()) *src) {
  __builtin_trivially_relocate(dest, src, 1);
}
void transfer_disjoint(decltype(closure()) *dest, decltype(closure()) *src) {
  __builtin_trivially_relocate(dest, src, 1, true);
}
#endif
