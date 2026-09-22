// Every type_info is constant data of the image that uses it: read-only from
// load, before any constructor runs, in the executable, in a DLL and in the
// shared C++ runtime, and type identity across the images is intact.
// RUN: %clangxx_crt_dll -std=c++17 -O2 -UNDEBUG -DBUILD_DLL %s -o %t.dll -Wl,-implib:%t.lib
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s %t.lib -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <exception>
#include <typeinfo>
#include <windows.h>

#if defined(BUILD_DLL)
#  define API __declspec(dllexport)
#else
#  define API __declspec(dllimport)
#endif

struct API Base {
  virtual ~Base();
  virtual int value() const;
};
struct API Derived : Base {
  int value() const override;
};
API Base *makeDerived();
API const std::type_info &dynamicType(const Base &);
API void throwDerived();
API bool dllSealedBeforeConstructors();

static bool readOnly(const void *Pointer) {
  MEMORY_BASIC_INFORMATION Info;
  if (VirtualQuery(Pointer, &Info, sizeof(Info)) != sizeof(Info))
    return false;
  return Info.State == MEM_COMMIT && Info.Protect == PAGE_READONLY;
}

#if defined(BUILD_DLL)
Base::~Base() {}
int Base::value() const { return 1; }
int Derived::value() const { return 2; }
Base *makeDerived() { return new Derived; }
const std::type_info &dynamicType(const Base &Object) { return typeid(Object); }
void throwDerived() { throw Derived(); }

// Private to the DLL: a COMDAT type_info.
struct Hidden : Base {};
static bool SealedBeforeConstructors =
    readOnly(&typeid(Hidden)) && readOnly(&typeid(Derived));
bool dllSealedBeforeConstructors() { return SealedBeforeConstructors; }
#else
struct Local : Derived {
  int value() const override { return 3; }
};

// The earliest constructor priority already sees read-only RTTI in this
// image, for a class defined in the DLL, and in the C++ runtime.
struct Probe {
  bool Sealed = readOnly(&typeid(Local)) && readOnly(&typeid(Base)) &&
                readOnly(&typeid(Base *)) && readOnly(&typeid(int)) &&
                readOnly(&typeid(std::exception)) &&
                typeid(Local) != typeid(Derived);
};
[[gnu::init_priority(101)]] static Probe SealedBeforeConstructors;

extern "C" void *__dynamic_cast(const void *, const void *, const void *,
                                ptrdiff_t);

int main() {
  assert(SealedBeforeConstructors.Sealed);
  assert(dllSealedBeforeConstructors());

  // dynamic_cast is a direct call into this image, not the shared runtime.
  assert(reinterpret_cast<void *>(
             GetProcAddress(GetModuleHandleA("libc++.dll"), "__dynamic_cast")) !=
         reinterpret_cast<void *>(&__dynamic_cast));

  // Identity across images: the executable and the DLL agree on both of
  // their descriptors, and dynamic_cast works in both directions.
  Base *Object = makeDerived();
  assert(typeid(*Object) == typeid(Derived));
  assert(dynamicType(*Object) == typeid(Derived));
  assert(dynamic_cast<Derived *>(Object)->value() == 2);
  Local Own;
  assert(dynamicType(Own) == typeid(Local));
  assert(dynamic_cast<Local *>(static_cast<Base *>(&Own))->value() == 3);
  delete Object;

  // Catch matching walks the imported descriptors.
  bool Caught = false;
  try {
    throwDerived();
  } catch (const Base &Exception) {
    Caught = typeid(Exception) == typeid(Derived) && Exception.value() == 2;
  }
  assert(Caught);
  return 0;
}
#endif
