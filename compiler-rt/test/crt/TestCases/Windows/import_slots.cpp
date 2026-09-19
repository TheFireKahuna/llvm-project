// Static data that holds the address of a symbol from another DLL is filled
// by the loader in place: pointer identity holds for data and functions, a
// pointer into the middle of an imported array receives its addend from the
// startup code, which leaves read-only data read-only, and a vtable entry
// for an inherited imported virtual function holds the function's address.
// The declarations are marked as on ELF; a __declspec(dllimport) address is
// not a constant to the front end and would be initialised at run time.
//
// RUN: %clangxx_crt -DBUILD_DLL -shared %s -o %t.dll -Wl,-implib:%t.lib
// RUN: %clangxx_crt_main_cfg %s %t.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <windows.h>

#define DLL_API __attribute__((visibility("default")))

extern "C" {
extern DLL_API int dll_array[8];
DLL_API int dll_func(int);
DLL_API int *dll_array_address();
DLL_API int (*dll_func_address())(int);
}

class DLL_API Base {
public:
  virtual ~Base();
  virtual int value() const;
  virtual int inherited() const;
};

#ifdef BUILD_DLL

int dll_array[8] = {0, 10, 20, 30, 40, 50, 60, 70};
int dll_func(int X) { return X * 2; }
int *dll_array_address() { return dll_array; }
int (*dll_func_address())(int) { return dll_func; }

Base::~Base() = default;
int Base::value() const { return 100; }
int Base::inherited() const { return 200; }

#else

// Read-only slots.
int *const ro_element = &dll_array[3];
int (*const ro_func)(int) = dll_func;
// Writable slots.
int *rw_element = &dll_array[5];
int (*rw_func)(int) = dll_func;

// Local's vtable, in this image's read-only data, holds Base::inherited.
class Local : public Base {
public:
  int value() const override { return 7; }
};

int main() {
  // CHECK: identity: data 1, function 1
  printf("identity: data %d, function %d\n",
         ro_element == dll_array_address() + 3 &&
             rw_element == dll_array_address() + 5,
         ro_func == dll_func_address() && rw_func == dll_func_address());
  // CHECK: values: 30 50 4 6
  printf("values: %d %d %d %d\n", *ro_element, *rw_element, ro_func(2),
         rw_func(3));

  Local L;
  Base *B = &L;
  // CHECK: virtual: 7 200
  printf("virtual: %d %d\n", B->value(), B->inherited());

  MEMORY_BASIC_INFORMATION Info;
  VirtualQuery(&ro_element, &Info, sizeof(Info));
  // CHECK: read-only slots: read-only
  printf("read-only slots: %s\n",
         Info.Protect == PAGE_READONLY ? "read-only" : "writable");
  return 0;
}

#endif

// The DLL's own descriptor, then one per run of slots: the two writable
// slots that are adjacent, and the read-only ones.
// IMPORTS:     Name: import_slots.cpp.tmp.dll
// IMPORTS:     Name: import_slots.cpp.tmp.dll
// IMPORTS:     Name: import_slots.cpp.tmp.dll
