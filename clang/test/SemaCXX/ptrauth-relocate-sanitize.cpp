// RUN: %clang_cc1 -triple arm64 -fptrauth-calls -fptrauth-intrinsics -std=c++23 -verify=plain %s
// RUN: %clang_cc1 -triple arm64 -fptrauth-calls -fptrauth-intrinsics -std=c++23 -fsanitize=address -fsanitize-address-field-padding=1 -verify=asan %s
// RUN: %clang_cc1 -triple arm64 -fptrauth-calls -fptrauth-intrinsics -std=c++23 -fsanitize=address -fsanitize-address-field-padding=2 -verify=asan %s
// RUN: %clang_cc1 -triple arm64 -fptrauth-calls -fptrauth-intrinsics -std=c++23 -fexperimental-pointer-field-protection-tagged -verify=asan %s
// plain-no-diagnostics

struct [[clang::trivially_relocatable(true)]] Owner {
  void *__ptrauth(1, 1, 42) pointer;
  Owner(Owner &&);
  ~Owner();
private:
  int value;
};
struct Nested { Owner owners[2]; };

void transfer(Owner *dest, Owner *src) {
  __builtin_trivially_relocate(dest, src, 1); // asan-error {{must be relocatable}}
}
void transfer(Nested *dest, Nested *src) {
  __builtin_trivially_relocate(dest, src, 1, false); // asan-error {{must be relocatable}}
}
