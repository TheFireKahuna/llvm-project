// Test vtable pseudo-relocations across DLL boundary.
// The pseudo-reloc runtime patches vtable pointers after IAT resolution.
//
// RUN: %clangxx_crt -DBUILD_DLL -shared %s -o %t.dll -Wl,-implib:%t.lib
// RUN: %clangxx_crt_main %s %t.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

#ifdef BUILD_DLL
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT __declspec(dllimport)
#endif

// Base class exported from DLL with virtual methods.
class DLL_EXPORT Base {
public:
  virtual ~Base() = default;
  virtual int getValue() const { return 100; }
  virtual const char *getName() const { return "Base"; }
};

// Derived class also in DLL.
class DLL_EXPORT Derived : public Base {
public:
  int getValue() const override { return 200; }
  const char *getName() const override { return "Derived"; }
};

#ifdef BUILD_DLL

// Factory functions exported from DLL.
extern "C" DLL_EXPORT Base *createBase() { return new Base(); }
extern "C" DLL_EXPORT Base *createDerived() { return new Derived(); }
extern "C" DLL_EXPORT void destroy(Base *p) { delete p; }

#else

// Import factory functions.
extern "C" Base *createBase();
extern "C" Base *createDerived();
extern "C" void destroy(Base *p);

// Local derived class extending imported base.
class LocalDerived : public Base {
public:
  int getValue() const override { return 300; }
  const char *getName() const override { return "LocalDerived"; }
};

int main() {
  // CHECK: DLL vtable test
  printf("DLL vtable test\n");

  // Test object created in DLL.
  Base *b = createBase();
  // CHECK: Base: getValue=100, name=Base
  printf("Base: getValue=%d, name=%s\n", b->getValue(), b->getName());
  destroy(b);

  // Test derived object created in DLL.
  Base *d = createDerived();
  // CHECK: Derived: getValue=200, name=Derived
  printf("Derived: getValue=%d, name=%s\n", d->getValue(), d->getName());
  destroy(d);

  // Test local class deriving from imported base.
  // This exercises pseudo-reloc patching of the vtable pointer.
  LocalDerived ld;
  Base *ldp = &ld;
  // CHECK: LocalDerived: getValue=300, name=LocalDerived
  printf("LocalDerived: getValue=%d, name=%s\n", ldp->getValue(), ldp->getName());

  // Verify polymorphism works correctly.
  Base *objects[] = {createBase(), createDerived(), &ld};
  // CHECK: Polymorphic calls:
  printf("Polymorphic calls:\n");
  for (int i = 0; i < 3; ++i) {
    // CHECK: obj{{.*}}: getValue={{[0-9]+}}, name={{.*}}
    printf("  obj[%d]: getValue=%d, name=%s\n", i,
           objects[i]->getValue(), objects[i]->getName());
  }
  destroy(objects[0]);
  destroy(objects[1]);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}

#endif
