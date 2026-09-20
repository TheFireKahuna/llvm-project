// Unannotated code consumed with -fauto-import reaches the address point of a
// vtable another image exports by name, as it does a marked one, so a
// constant-initialised object of the class holds a value the loader fills and
// the image needs no start-up fixing at all.
//
// RUN: %clangxx_crt -DBUILD_DLL -shared -mdefault-visibility-export-mapping=all %s -o %t.dll -Wl,-implib:%t.import.lib
// RUN: %clangxx_crt_main -fauto-import %s %t.import.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS
//
// REQUIRES: windows, crt

#include <stdio.h>

// No visibility attribute and no __declspec anywhere.
class Plain {
public:
  virtual int value() const;
};

#ifdef BUILD_DLL

int Plain::value() const { return 7; }
Plain *makePlain() { return new Plain(); }

#else

Plain *makePlain();

// A class of this image whose key function is defined after an object of it:
// the initialiser asks for the address point while the vtable is still a
// declaration, and the definition that follows has to replace what the
// reference left behind rather than skip a name that is already taken.
struct Late {
  virtual int value() const;
};
const Late LateObject;
int Late::value() const { return 9; }

const Plain RoObject;
Plain RwObject;

struct ImportFixup {
  unsigned Rva, Flags;
  long long Addend;
};
extern "C" const ImportFixup __import_fixups_start[], __import_fixups_end[];

static const void *vptrOf(const void *Object) {
  return *reinterpret_cast<const void *const *>(Object);
}

int main() {
  Plain *FromDll = makePlain();
  // CHECK: values: 7 7 7
  printf("values: %d %d %d\n", RoObject.value(), RwObject.value(),
         FromDll->value());
  // CHECK-NEXT: one vtable: 1 1
  printf("one vtable: %d %d\n", vptrOf(&RoObject) == vptrOf(&RwObject),
         vptrOf(&RoObject) == vptrOf(FromDll));
  // CHECK-NEXT: defined here: 9
  printf("defined here: %d\n", LateObject.value());

  // CHECK-NEXT: fixups: 0
  printf("fixups: %d\n", (int)(__import_fixups_end - __import_fixups_start));
  return 0;
}

#endif

// The address point is what the executable imports.
// IMPORTS: Symbol: _ZTV5Plain$ap16
