// Test basic RTTI (Run-Time Type Information) support.
//
// RUN: %clang_crt_main -std=c++17 -frtti %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <typeinfo>

class Animal {
public:
  virtual void speak() = 0;
  virtual ~Animal() = default;
};

class Dog : public Animal {
public:
  void speak() override {
    printf("Dog says: Woof!\n");
  }
};

class Cat : public Animal {
public:
  void speak() override {
    printf("Cat says: Meow!\n");
  }
};

class Labrador : public Dog {
public:
  void speak() override {
    printf("Labrador says: Woof woof!\n");
  }
};

void identify(Animal *a) {
  printf("typeid name: %s\n", typeid(*a).name());

  if (dynamic_cast<Labrador *>(a)) {
    printf("  -> is a Labrador\n");
  } else if (dynamic_cast<Dog *>(a)) {
    printf("  -> is a Dog\n");
  } else if (dynamic_cast<Cat *>(a)) {
    printf("  -> is a Cat\n");
  }
}

int main() {
  // CHECK: RTTI basic test
  printf("RTTI basic test\n");

  Dog dog;
  Cat cat;
  Labrador lab;

  // CHECK: Testing dog:
  printf("Testing dog:\n");
  identify(&dog);
  // CHECK: -> is a Dog

  // CHECK: Testing cat:
  printf("Testing cat:\n");
  identify(&cat);
  // CHECK: -> is a Cat

  // CHECK: Testing labrador:
  printf("Testing labrador:\n");
  identify(&lab);
  // CHECK: -> is a Labrador

  // Type comparison.
  Animal *a1 = &dog;
  Animal *a2 = &lab;

  // CHECK: typeid comparison dog vs labrador: different
  printf("typeid comparison dog vs labrador: %s\n",
         typeid(*a1) == typeid(*a2) ? "same" : "different");

  // CHECK: typeid comparison labrador vs labrador: same
  Labrador lab2;
  Animal *a3 = &lab2;
  printf("typeid comparison labrador vs labrador: %s\n",
         typeid(*a2) == typeid(*a3) ? "same" : "different");

  // type_info address comparison (should be consistent).
  const std::type_info &ti1 = typeid(*a2);
  const std::type_info &ti2 = typeid(*a3);
  // CHECK: type_info address match: 1
  printf("type_info address match: %d\n", &ti1 == &ti2);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
