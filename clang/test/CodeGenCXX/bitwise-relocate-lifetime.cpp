// RUN: %clang_cc1 -std=c++23 -triple x86_64-linux-gnu -O2 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -std=c++26 -triple x86_64-windows-msvc -O2 -emit-llvm -o - %s | FileCheck %s

using size_t = __SIZE_TYPE__;
void *operator new(size_t, void *p) noexcept { return p; }
void *operator new[](size_t, void *p) noexcept { return p; }

struct [[clang::trivially_relocatable(true)]] Owner {
  int *releases;
  explicit Owner(int *p) : releases(p) {}
  Owner(Owner&& other) : releases(other.releases) { other.releases = nullptr; }
  ~Owner() {
    if (releases)
      ++*releases;
  }
};

// Raw storage prevents automatic source destruction after its lifetime ends.
// Destroy only the relocated objects, exactly once.
int objects() {
  int releases = 0;
  alignas(Owner) unsigned char source[sizeof(Owner)];
  alignas(Owner) unsigned char destination[sizeof(Owner)];
  Owner *src = new (source) Owner(&releases);
  Owner *dest = __builtin_trivially_relocate((Owner*)destination, src, 1, true);
  if (dest != (Owner*)destination || dest->releases != &releases || releases)
    return 1;
  // Reusing dead source storage must not change destination ownership.
  __builtin_memset(source, 0, sizeof(source));
  dest->~Owner();
  return releases != 1;
}

int arrays() {
  int releases = 0;
  alignas(Owner) unsigned char source[3 * sizeof(Owner)];
  alignas(Owner) unsigned char destination[3 * sizeof(Owner)];
  Owner *src = new (source) Owner[3]{Owner(&releases), Owner(&releases), Owner(&releases)};
  Owner *dest = __builtin_trivially_relocate((Owner*)destination, src, 3, true);
  if (releases)
    return 1;
  for (int i = 0; i != 3; ++i) {
    if (dest[i].releases != &releases)
      return 2;
    dest[i].~Owner();
  }
  return releases != 3;
}

int closures() {
  int releases = 0;
  int observed = 42;
  auto prototype = [owner = Owner(&releases), &observed] { return observed; };
  using Closure = decltype(prototype);
  static_assert(__builtin_is_bitwise_relocatable(Closure));
  static_assert(!__is_assignable(Closure&, Closure&&));
  alignas(Closure) unsigned char source[sizeof(Closure)];
  alignas(Closure) unsigned char destination[sizeof(Closure)];
  Closure *src = new (source) Closure(static_cast<Closure&&>(prototype));
  Closure *dest = __builtin_trivially_relocate((Closure*)destination, src, 1, true);
  if (releases || (*dest)() != 42)
    return 1;
  observed = 73;
  if ((*dest)() != 73)
    return 2;
  dest->~Closure();
  return releases != 1;
}

// Vacate the destination-only element before shifting live owners in either
// direction. Overlapping source objects become live destination objects;
// source-only storage must not be destroyed afterwards.
int overlap(bool backward) {
  int releases[4] = {};
  alignas(Owner) unsigned char storage[4 * sizeof(Owner)];
  Owner *values = new (storage) Owner[4]{Owner(&releases[0]), Owner(&releases[1]),
                                       Owner(&releases[2]), Owner(&releases[3])};
  values[backward ? 3 : 0].~Owner();
  Owner *dest = backward ? __builtin_trivially_relocate(values + 1, values, 3)
                         : __builtin_trivially_relocate(values, values + 1, 3, false);
  if (dest != values + (backward ? 1 : 0))
    return 1;
  for (int i = 0; i != 3; ++i) {
    if (dest[i].releases != &releases[i + (backward ? 0 : 1)])
      return 2;
    dest[i].~Owner();
  }
  for (int released : releases)
    if (released != 1)
      return 3;
  return 0;
}

int self() {
  int releases = 0;
  alignas(Owner) unsigned char storage[sizeof(Owner)];
  Owner *value = new (storage) Owner(&releases);
  if (__builtin_trivially_relocate(value, value, 1) != value || releases)
    return 1;
  value->~Owner();
  return releases != 1;
}

// The original disjoint lifetime checks still fold completely after migrating
// to the existing builtin. Overlapping transfers are also exercised by main.
// CHECK-LABEL: define {{.*}}i32 @disjoint_lifetimes(
// CHECK-NEXT: {{.*}}:
// CHECK-NEXT: ret i32 0
extern "C" int disjoint_lifetimes() {
  if (__builtin_trivially_relocate((Owner*)nullptr, (Owner*)nullptr, 0, true) != nullptr)
    return 10;
  if (__builtin_trivially_relocate((Owner*)nullptr, (Owner*)nullptr, 0) != nullptr)
    return 11;
  return objects() || arrays() || closures();
}

int main() {
  return disjoint_lifetimes() || overlap(false) || overlap(true) || self();
}
