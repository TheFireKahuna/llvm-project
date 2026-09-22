// RUN: %clang_cc1 -std=c++14 -verify %s
// RUN: %clang_cc1 -std=c++23 -verify %s
// RUN: %clang_cc1 -std=c++26 -verify %s

static_assert(__has_builtin(__builtin_is_bitwise_relocatable), "");
static_assert(__has_builtin(__builtin_trivially_relocate), "");
static_assert(!__has_builtin(__builtin_bitwise_relocate), "");
static_assert(__has_cpp_attribute(clang::trivially_relocatable), "");
static_assert(__builtin_is_bitwise_relocatable(int), "");
static_assert(__builtin_is_bitwise_relocatable(const int[3]), "");
static_assert(!__builtin_is_bitwise_relocatable(volatile int), "");
static_assert(!__builtin_is_bitwise_relocatable(void), "");
static_assert(!__builtin_is_bitwise_relocatable(int&), "");
static_assert(!__builtin_is_bitwise_relocatable(int[]), "");

struct [[clang::trivially_relocatable(true)]] Owner {
  int *pointer;
  Owner(int * = nullptr);
  Owner(Owner&&);
  ~Owner();
};
static_assert(__builtin_is_bitwise_relocatable(Owner), "");
static_assert(__builtin_is_bitwise_relocatable(Owner[2]), "");
static_assert(!__builtin_is_bitwise_relocatable(const Owner), "");
static_assert(!__is_trivially_copyable(Owner), "");
static_assert(!__can_pass_in_regs(Owner), "");
static_assert(!__is_trivially_relocatable(Owner), ""); // expected-warning {{deprecated}}
static_assert(!__builtin_is_cpp_trivially_relocatable(Owner), "");

template<bool Enable>
struct [[clang::trivially_relocatable(Enable)]] Conditional {
  int *pointer;
  Conditional(Conditional&&);
  ~Conditional();
};
static_assert(__builtin_is_bitwise_relocatable(Conditional<true>), "");
static_assert(!__builtin_is_bitwise_relocatable(Conditional<false>), "");

struct [[clang::trivially_relocatable(false)]] OptOut { int value; };
static_assert(!__builtin_is_bitwise_relocatable(OptOut), "");
static_assert(__is_trivially_copyable(OptOut), "");

struct [[clang::trivially_relocatable(true), clang::trivially_relocatable(false)]] Multiple {};
struct [[clang::trivially_relocatable(false)]] Redeclared;
struct [[clang::trivially_relocatable(true)]] Redeclared {};
static_assert(!__builtin_is_bitwise_relocatable(Multiple), "");
static_assert(!__builtin_is_bitwise_relocatable(Redeclared), "");

template<bool Enable>
struct [[clang::trivially_relocatable(Enable)]] Forward;
template<bool Enable>
struct [[clang::trivially_relocatable(true)]] Forward {};
static_assert(!__builtin_is_bitwise_relocatable(Forward<false>), "");
static_assert(__builtin_is_bitwise_relocatable(Forward<true>), "");

struct Composite : Owner { Composite(int&); Owner elements[2]; int& reference; };
static_assert(__builtin_is_bitwise_relocatable(Composite), "");
static_assert(!__is_assignable(Composite&, Composite&&), "");
struct ConstOwner { const Owner owner; };
static_assert(!__builtin_is_bitwise_relocatable(ConstOwner), "");
struct Volatile { volatile int value; };
struct [[clang::trivially_relocatable(true)]] WarrantedVolatile { Volatile value; };
static_assert(!__builtin_is_bitwise_relocatable(WarrantedVolatile), "");

struct ObservableMove {
  ObservableMove(ObservableMove&&);
  ObservableMove(const ObservableMove&) = default;
};
static_assert(!__builtin_is_bitwise_relocatable(ObservableMove), "");
struct ObservableDtor { ~ObservableDtor(); };
static_assert(!__builtin_is_bitwise_relocatable(ObservableDtor), "");
struct DeletedMove {
  DeletedMove(DeletedMove&&) = delete;
  DeletedMove(const DeletedMove&) = default;
};
static_assert(!__builtin_is_bitwise_relocatable(DeletedMove), "");
struct [[clang::trivially_relocatable(true)]] DeletedDtor { ~DeletedDtor() = delete; };
static_assert(!__builtin_is_bitwise_relocatable(DeletedDtor), "");
struct [[clang::trivially_relocatable(true)]] Polymorphic { virtual void f(); };
struct [[clang::trivially_relocatable(true)]] VirtualBase : virtual Owner {};
static_assert(!__builtin_is_bitwise_relocatable(Polymorphic), "");
static_assert(!__builtin_is_bitwise_relocatable(VirtualBase), "");
union TrivialUnion { int value; double other; };
static_assert(__builtin_is_bitwise_relocatable(TrivialUnion), "");

void closures(int& reference, const Owner& owner) {
  auto trivial = [value = 1] { return value; };
  auto owning = [value = Owner{}] { return value.pointer; };
  auto by_reference = [&reference] { return reference; };
  auto observable = [value = ObservableDtor{}] {};
  static_assert(__builtin_is_bitwise_relocatable(decltype(trivial)), "");
  static_assert(__builtin_is_bitwise_relocatable(decltype(owning)), "");
  static_assert(__builtin_is_bitwise_relocatable(decltype(by_reference)), "");
  static_assert(!__builtin_is_bitwise_relocatable(decltype(observable)), "");
  static_assert(!__is_assignable(decltype(owning)&, decltype(owning)&&), "");
}

struct Incomplete; // expected-note {{forward declaration}}
bool incomplete = __builtin_is_bitwise_relocatable(Incomplete); // expected-error {{incomplete type}}
int condition; // expected-note {{declared here}}
struct [[clang::trivially_relocatable(condition)]] NonConstant {}; // expected-error {{not an integral constant expression}} expected-note {{read of non-const variable}}
struct [[clang::trivially_relocatable()]] Missing {}; // expected-error {{takes one argument}}
[[clang::trivially_relocatable(true)]] int wrong_subject; // expected-error {{only applies to}}

void operations(Owner *dest, Owner *src, const Owner *c, Volatile *v) {
  static_assert(noexcept(__builtin_trivially_relocate(dest, src, 1)), "");
  static_assert(noexcept(__builtin_trivially_relocate(dest, src, 1, true)), "");
  __builtin_trivially_relocate(dest, src, 1);
  __builtin_trivially_relocate(dest, src, 1, false);
  Owner *const const_dest = dest;
  Owner *const const_src = src;
  __builtin_trivially_relocate(const_dest, const_src, 1, true);
  static_assert(__is_same(decltype(__builtin_trivially_relocate(dest, src, 1, true)), Owner*), "");
  __builtin_trivially_relocate(dest, src, 1, true);
  __builtin_trivially_relocate(c, c, 1, true); // expected-error {{must be non-const}}
  __builtin_trivially_relocate(v, v, 1, true); // expected-error {{must be relocatable}}
  __builtin_trivially_relocate(dest, c, 1, true); // expected-error {{must be of the same type}}
  __builtin_trivially_relocate(1, src, 1, true); // expected-error {{must be a pointer}}
}

template<bool Disjoint>
Owner *transfer(Owner *dest, Owner *src) {
  return __builtin_trivially_relocate(dest, src, 1, Disjoint);
}
template Owner *transfer<true>(Owner*, Owner*);
template Owner *transfer<false>(Owner*, Owner*);

// Opting out of the bitwise extension does not change legacy eligibility.
void opted_out(OptOut *dest, OptOut *src) {
  __builtin_trivially_relocate(dest, src, 1);
  __builtin_trivially_relocate(dest, src, 1, false);
  __builtin_trivially_relocate(dest, src, 1, true); // expected-error {{must be relocatable}}
}

// Recheck eligibility after a dependent mode becomes a disjoint request.
template<bool Disjoint>
void opted_out_template(OptOut *dest, OptOut *src) {
  __builtin_trivially_relocate(dest, src, 1, Disjoint); // expected-error {{must be relocatable}}
}
template void opted_out_template<false>(OptOut*, OptOut*);
template void opted_out_template<true>(OptOut*, OptOut*); // expected-note {{in instantiation}}

void invalid_mode(Owner *dest, Owner *src, bool mode) {
  __builtin_trivially_relocate(dest, src, 1, mode); // expected-error {{must be a constant integer}}
  __builtin_trivially_relocate(dest, src, 1, 2); // expected-error {{outside the valid range}}
  __builtin_trivially_relocate(dest, src, 1, -1); // expected-error {{outside the valid range}}
  __builtin_trivially_relocate(dest, src, 1, 1.0); // expected-error {{must be a constant integer}}
  // An unevaluated call still requires a constant mode.
  using Result = decltype(__builtin_trivially_relocate(dest, src, 1, mode)); // expected-error {{must be a constant integer}}
}
