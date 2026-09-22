// RUN: %clang_cc1 -triple arm64 -fptrauth-calls -fptrauth-intrinsics -std=c++23 -verify %s
// RUN: %clang_cc1 -triple arm64 -fptrauth-calls -fptrauth-intrinsics -std=c++26 -verify %s

struct [[clang::trivially_relocatable(true)]] AddressDiscriminated {
  void * __ptrauth(1, 1, 1234) pointer;
};
struct NonAddressDiscriminated {
  void * __ptrauth(1, 0, 1234) pointer;
};
struct [[clang::trivially_relocatable(true)]] Nested {
  AddressDiscriminated values[2];
};
static_assert(!__builtin_is_bitwise_relocatable(AddressDiscriminated));
static_assert(!__builtin_is_bitwise_relocatable(Nested));
static_assert(!__is_bitwise_cloneable(Nested));
static_assert(!__builtin_is_cpp_trivially_relocatable(Nested));
static_assert(__builtin_is_bitwise_relocatable(NonAddressDiscriminated));

void transfer(AddressDiscriminated *dest, AddressDiscriminated *src) {
  __builtin_trivially_relocate(dest, src, 1);
  __builtin_trivially_relocate(dest, src, 1, false);
  __builtin_trivially_relocate(dest, src, 1, true); // expected-error {{must be relocatable}}
}

void transfer(NonAddressDiscriminated *dest, NonAddressDiscriminated *src) {
  __builtin_trivially_relocate(dest, src, 1);
  __builtin_trivially_relocate(dest, src, 1, true);
}

struct [[clang::trivially_relocatable(true)]] Owner {
  void *__ptrauth(1, 1, 42) pointer;
  Owner();
  Owner(Owner &&);
  ~Owner();
};
struct Derived : Owner { Nested member; };
void transfer(Derived *dest, Derived *src, __SIZE_TYPE__ count) {
  __builtin_trivially_relocate(dest, src, count);
  __builtin_trivially_relocate(dest, src, count, true); // expected-error {{must be relocatable}}
}
void closure() {
  auto lambda = [owner = Owner{}] {};
  using T = decltype(lambda);
  static_assert(!__builtin_is_bitwise_relocatable(T));
  static_assert(__is_same(decltype(__builtin_trivially_relocate((T *)0, (T *)0, 0)), T *));
}

// An explicit permission does not identify the active member of a union.
union [[clang::trivially_relocatable(true)]] UnknownActive {
  Owner owner;
  int value;
  ~UnknownActive();
};
struct [[clang::trivially_relocatable(true)]] ContainsUnion { UnknownActive value; };
struct [[clang::trivially_relocatable(false)]] OptOut : Owner {};
struct ObservableMove : Owner { ObservableMove(ObservableMove &&); };
struct Volatile { volatile AddressDiscriminated value; };
struct ConstOwner { const Owner value; };
struct [[clang::trivially_relocatable(true)]] Polymorphic : Owner { virtual void f(); };
struct [[clang::trivially_relocatable(true)]] VirtualBase : virtual Owner {};
struct [[clang::trivially_relocatable(true)]] DeletedDestructor : Owner { ~DeletedDestructor() = delete; };

template<class T> void invalid(T *dest, T *src) {
  __builtin_trivially_relocate(dest, src, 1); // expected-error 8 {{must be relocatable}}
}
template void invalid(UnknownActive *, UnknownActive *); // expected-note {{in instantiation}}
template void invalid(ContainsUnion *, ContainsUnion *); // expected-note {{in instantiation}}
template void invalid(OptOut *, OptOut *); // expected-note {{in instantiation}}
template void invalid(ObservableMove *, ObservableMove *); // expected-note {{in instantiation}}
template void invalid(Volatile *, Volatile *); // expected-note {{in instantiation}}
template void invalid(ConstOwner *, ConstOwner *); // expected-note {{in instantiation}}
template void invalid(Polymorphic *, Polymorphic *); // expected-note {{in instantiation}}
template void invalid(VirtualBase *, VirtualBase *); // expected-note {{in instantiation}}
void deleted_destructor(DeletedDestructor *dest, DeletedDestructor *src) {
  __builtin_trivially_relocate(dest, src, 1); // expected-error {{must be relocatable}}
}
