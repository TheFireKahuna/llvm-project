// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -DRENAME \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -DOPAQUE_ENUM \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -DWIDE \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=WIDE %s

// The digest of an ABI contract covers the layout of the class and nothing
// else: a field's name does not change it, and a field more does. The locator,
// the first 11 characters, is the hash of the class's name alone. The digest is
// pinned, since an image built by one compiler binds to one another compiler
// built. An
// enumeration is its underlying type, whether the unit defines it or only
// declares it with a fixed one.

#define API __attribute__((visibility("default")))

struct Base {
  virtual ~Base();
  int b;
};

struct API Pinned : Base {
  int first;
#ifdef WIDE
  int added;
#endif
#ifdef RENAME
  short renamed : 3;
#else
  short bits : 3;
#endif
  double d;
  API void f();
};

void Pinned::f() {}

#ifdef OPAQUE_ENUM
enum class Kind : short;
#else
enum class Kind : short { A, B };
#endif

struct API WithEnum {
  Kind k;
  API void g();
};

void WithEnum::g() {}

// SAME: !{!"/abiprovide:bYVgnX5b3082C4uyG6tcxDE6hrq6WDBbg,_JxiUs3TwoUWTmcUCigZPOZB4ND2-E-Pg"}
// WIDE: !{!"/abiprovide:bYVgnX5b308pa8Ei5rlFv6O0NNUzIZN_Q,_JxiUs3TwoUWTmcUCigZPOZB4ND2-E-Pg"}
