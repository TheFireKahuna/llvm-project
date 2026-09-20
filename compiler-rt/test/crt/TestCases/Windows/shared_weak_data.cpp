// A COMDAT variable marked as crossing the shared-library boundary -- an
// inline variable, a template static data member, a static local of an inline
// function -- gets one instance for the program, as it does on ELF, rather
// than one per image. The image that links a DLL offering a copy binds to it
// and drops its own, along with the initializer the compiler put in the same
// COMDAT, and forwards its own export of the name to the DLL.
//
// RUN: %clangxx_crt -DBUILD_DLL -shared %s -o %t.dll -Wl,-implib:%t.lib
// RUN: %clangxx_crt_main %s %t.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-exports %t.exe | FileCheck %s --check-prefix=FORWARD
//
// REQUIRES: windows, crt

#include <stdio.h>

#define API __attribute__((visibility("default")))

API extern int Constructions;

struct Counted {
  Counted() : Value(++Constructions) {}
  int Value;
};

API inline int InlineVariable = 7;

template <class T> struct API Holder {
  static Counted Value;
};
template <class T> Counted Holder<T>::Value;

API inline Counted &instance() {
  static Counted One;
  return One;
}

API int *inlineVariableAddress();
API int holderValue();
API int instanceValue();
API int constructions();

#ifdef BUILD_DLL

int Constructions;

int *inlineVariableAddress() { return &InlineVariable; }
int holderValue() { return Holder<int>::Value.Value; }
int instanceValue() { return instance().Value; }
int constructions() { return Constructions; }

#else

int main() {
  // CHECK: inline variable: shared 1
  printf("inline variable: shared %d\n",
         &InlineVariable == inlineVariableAddress());

  InlineVariable = 42;
  // CHECK-NEXT: write is seen: 1
  printf("write is seen: %d\n", *inlineVariableAddress() == 42);

  // CHECK-NEXT: shared instances: 1 1
  printf("shared instances: %d %d\n",
         Holder<int>::Value.Value == holderValue(),
         instance().Value == instanceValue());

  // One construction for the template static, one for the function-local
  // static that the calls above reached; this image contributed neither,
  // and its guard is the DLL's too.
  // CHECK-NEXT: constructions: 2 2
  printf("constructions: %d %d\n", Constructions, constructions());
  return 0;
}

#endif

// The executable exports the names it compiled, and each one names the DLL
// that holds the instance.
// FORWARD: Name: InlineVariable
// FORWARD-NEXT: ForwardedTo: shared_weak_data.cpp.tmp.InlineVariable
