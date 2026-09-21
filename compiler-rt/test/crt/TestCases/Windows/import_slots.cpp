// Static data that holds the address of a symbol from another DLL is filled
// by the loader in place: pointer identity holds for data and functions, a
// pointer into the middle of an imported array receives its addend from the
// startup code, which leaves read-only data read-only, and a vtable entry
// for an inherited imported virtual function holds the function's address.
// Declarations marked as on ELF and __declspec(dllimport) ones behave alike:
// both addresses are constants to the front end.
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
#ifdef BUILD_DLL
#define DLL_SPEC __declspec(dllexport)
#else
#define DLL_SPEC __declspec(dllimport)
#endif

extern "C" {
extern DLL_API int dll_array[8];
DLL_API int dll_func(int);
DLL_API int *dll_array_address();
DLL_API int (*dll_func_address())(int);
extern DLL_SPEC int spec_array[8];
DLL_SPEC int spec_func(int);
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
int spec_array[8] = {0, 11, 22, 33, 44, 55, 66, 77};
int spec_func(int X) { return X * 3; }

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
// The same through __declspec(dllimport); external linkage keeps the
// read-only slot from being folded into its only use.
int *spec_element = &spec_array[3];
extern int (*const spec_func_ptr)(int);
int (*const spec_func_ptr)(int) = spec_func;

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
  // CHECK: dllimport: 33 6 1
  printf("dllimport: %d %d %d\n", *spec_element, spec_func_ptr(2),
         spec_element == spec_array + 3 && spec_func_ptr == spec_func);

  Local L;
  Base *B = &L;
  // CHECK: virtual: 7 200
  printf("virtual: %d %d\n", B->value(), B->inherited());

  // The inherited entry, after the two destructors and value, is the
  // exported function itself, not a thunk in this image.
  HMODULE Dll;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(dll_func_address()), &Dll);
  void *const *Vtable = *reinterpret_cast<void *const *const *>(B);
  // CHECK: vtable entry: exported function 1
  printf("vtable entry: exported function %d\n",
         Vtable[3] == reinterpret_cast<void *>(
                          GetProcAddress(Dll, "_ZNK4Base9inheritedEv")));

  MEMORY_BASIC_INFORMATION Info;
  VirtualQuery(&ro_element, &Info, sizeof(Info));
  // CHECK: read-only slots: read-only
  printf("read-only slots: %s\n",
         Info.Protect == PAGE_READONLY ? "read-only" : "writable");
  return 0;
}

#endif

// The DLL's own descriptor, then one per run of slots: the adjacent writable
// slots form one, the read-only ones follow, and Local's vtable entry is a run
// of its own.
// IMPORTS:     Name: import_slots.cpp.tmp.dll
// IMPORTS:     Name: import_slots.cpp.tmp.dll
// IMPORTS:     Name: import_slots.cpp.tmp.dll
// IMPORTS:     Name: import_slots.cpp.tmp.dll
// IMPORTS:     Name: import_slots.cpp.tmp.dll
// IMPORTS-NEXT: ImportLookupTableRVA:
// IMPORTS-NEXT: ImportAddressTableRVA:
// IMPORTS-NEXT: Symbol: _ZNK4Base9inheritedEv (0)
