// A class whose vtable lives in another image exports one symbol per address
// point of that vtable. A constant-initialised object of the class then holds
// the value of a pointer the loader fills rather than an offset from one, so
// nothing is added at start-up and the object stays in read-only memory.
//
// RUN: %clangxx_crt -DBUILD_DLL -shared %s -o %t.dll -Wl,-implib:%t.lib
// RUN: %clangxx_crt_main %s %t.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <windows.h>

#define DLL_API __attribute__((visibility("default")))

class DLL_API Base {
public:
  virtual int value() const;
};

class DLL_API Derived : public Base {
public:
  int value() const override;
};

static const void *vptrOf(const void *Object) {
  return *reinterpret_cast<const void *const *>(Object);
}

#ifdef BUILD_DLL

int Base::value() const { return 1; }
int Derived::value() const { return 2; }

static const Base DllBase;
static const Derived DllDerived;

extern "C" DLL_API const void *baseAddressPoint() { return vptrOf(&DllBase); }
extern "C" DLL_API const void *derivedAddressPoint() {
  return vptrOf(&DllDerived);
}

#else

// Both are constant initialisers holding an address point of an imported
// vtable; the const one also has to survive in read-only memory.
const Base RoBase;
Derived RwDerived;

extern "C" const void *baseAddressPoint();
extern "C" const void *derivedAddressPoint();

int main() {
  // CHECK: address points: base 1, derived 1
  printf("address points: base %d, derived %d\n",
         vptrOf(&RoBase) == baseAddressPoint(),
         vptrOf(&RwDerived) == derivedAddressPoint());

  const Base *Objects[] = {&RoBase, &RwDerived};
  // CHECK: virtual: 1 2
  printf("virtual: %d %d\n", Objects[0]->value(), Objects[1]->value());

  // A slot needing an offset would be written at start-up and move the object
  // to writable data; an address point needs none.
  MEMORY_BASIC_INFORMATION Info;
  VirtualQuery(&RoBase, &Info, sizeof(Info));
  // CHECK: const object: read-only
  printf("const object: %s\n",
         Info.Protect == PAGE_READONLY ? "read-only" : "writable");
  return 0;
}

#endif

// The address points are what the executable imports; the vtables themselves
// are never named.
// IMPORTS: Symbol: _ZTV4Base$ap16
// IMPORTS: Symbol: _ZTV7Derived$ap16
