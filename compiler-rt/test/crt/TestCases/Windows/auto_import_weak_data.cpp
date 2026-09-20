// Under -fauto-import a COMDAT variable -- an inline variable, a template
// static data member, a function-local static of an inline function and its
// guard -- is left preemptable and reached through a pointer, so that the
// linker can bind every copy to one of them. With no other image offering a
// copy the linker rewrites those accesses back to this image's own, and the
// program behaves as it does without the flag.
//
// RUN: %clangxx_crt_main -fauto-import %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %clangxx_crt_main %s -o %t.local.exe
// RUN: %run %t.local.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

inline int InlineVariable = 7;

template <class T> struct Holder {
  static int Value;
};
template <class T> int Holder<T>::Value = 3;

static int Constructions;

struct Counted {
  Counted() : Value(++Constructions) {}
  int Value;
};

inline Counted &instance() {
  static Counted One;
  return One;
}

// A second translation-unit-like user of the same entities, so that each one
// has more than one reference for the linker to agree on.
int other() {
  return InlineVariable + Holder<int>::Value + instance().Value;
}

int main() {
  int *Address = &InlineVariable;
  // CHECK: values: 7 3 1
  printf("values: %d %d %d\n", InlineVariable, Holder<int>::Value,
         instance().Value);
  // CHECK: identity: 1, guard ran once: 1
  printf("identity: %d, guard ran once: %d\n", Address == &InlineVariable,
         Constructions == 1);
  // CHECK: other: 11
  printf("other: %d\n", other());
  return 0;
}
