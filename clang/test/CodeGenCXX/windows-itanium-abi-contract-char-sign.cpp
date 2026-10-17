// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-signed-char \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fwchar-type=short \
// RUN:   -fsigned-wchar -mdefault-visibility-export-mapping=explicit \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fwchar-type=int \
// RUN:   -fno-signed-wchar -mdefault-visibility-export-mapping=explicit \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=WIDER %s

// Whether plain char and wchar_t are signed does not change a contract: it is
// an option, which changes neither the type, its layout nor its mangling. A
// wchar_t of another size does, and signed and unsigned char, distinct types,
// stay distinct.

#define API __attribute__((visibility("default")))

enum class Small : char { A };

struct API Chars {
  char c;
  char bits : 4;
  Small s;
  wchar_t w;
  API void f();
};
void Chars::f() {}

struct API Signed {
  signed char c;
  API void f();
};
void Signed::f() {}

struct API Unsigned {
  unsigned char c;
  API void f();
};
void Unsigned::f() {}

// The rows of Chars, Signed and Unsigned, in that order.
// CHECK: !{!"/abiprovide:dH0T-csPZaUaQVDTodeYfDm3Eyg0FAjBg,T2JKrmYnm2c3YVML0QOOWU5gEqr1McDnA,3qnhPVEHJ9Q73eS6nuIf1MyTbowoBop5w"}
// WIDER: !{!"/abiprovide:dH0T-csPZaU8Ag2FGIuhpNmh0PZjJnCpw,T2JKrmYnm2c3YVML0QOOWU5gEqr1McDnA,3qnhPVEHJ9Q73eS6nuIf1MyTbowoBop5w"}
