// Type identity is the 128-bit hash the compiler stores in every type_info,
// so two images that each carry their own descriptor for a type agree on
// equality, hashing, ordering, dynamic_cast and catch matching, while the
// descriptors themselves live at different addresses.
// RUN: %clangxx_crt_dll -std=c++17 -O2 -UNDEBUG -DBUILD_DLL %s -o %t.dll -Wl,-implib:%t.lib
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s %t.lib -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

#if defined(BUILD_DLL)
#  define API __declspec(dllexport)
#else
#  define API __declspec(dllimport)
#endif

struct API Base {
  virtual ~Base();
};

// Instantiated separately in each image: each gets its own descriptor.
template <class T> struct Node : Base {
  T value{};
};

struct DllDescriptors {
  const std::type_info *nodeInt;
  const std::type_info *nodeIntPtr;
  const std::type_info *nodeChar;
  const std::type_info *base;
};
API DllDescriptors dllDescriptors();
API Base *dllMakeNodeInt();
API void dllThrowNodeChar();
API bool dllIsNodeInt(const Base &);

#if defined(BUILD_DLL)
Base::~Base() {}
DllDescriptors dllDescriptors() {
  return {&typeid(Node<int>), &typeid(Node<int> *), &typeid(Node<char>),
          &typeid(Base)};
}
Base *dllMakeNodeInt() { return new Node<int>; }
void dllThrowNodeChar() { throw Node<char>(); }
bool dllIsNodeInt(const Base &Object) {
  return typeid(Object) == typeid(Node<int>);
}
#else
int main() {
  DllDescriptors Dll = dllDescriptors();

  // Distinct copies, one identity.
  assert(Dll.nodeInt != &typeid(Node<int>));
  assert(*Dll.nodeInt == typeid(Node<int>));
  assert(Dll.nodeInt->hash_code() == typeid(Node<int>).hash_code());
  assert(!Dll.nodeInt->before(typeid(Node<int>)));
  assert(!typeid(Node<int>).before(*Dll.nodeInt));

  // Different types stay different, including pointer versus pointee.
  assert(*Dll.nodeInt != typeid(Node<char>));
  assert(*Dll.nodeIntPtr != typeid(Node<int>));
  assert(*Dll.nodeIntPtr == typeid(Node<int> *));
  assert(Dll.nodeInt->hash_code() != Dll.nodeChar->hash_code());
  assert(Dll.nodeInt->before(*Dll.nodeChar) != Dll.nodeChar->before(*Dll.nodeInt));

  // Even an imported class is described by a descriptor of this image.
  assert(Dll.base != &typeid(Base));
  assert(*Dll.base == typeid(Base));

  // type_index keyed containers resolve across images.
  std::unordered_map<std::type_index, int> Registry;
  Registry[typeid(Node<int>)] = 1;
  Registry[typeid(Node<char>)] = 2;
  assert(Registry.at(*Dll.nodeInt) == 1);
  assert(Registry.at(*Dll.nodeChar) == 2);

  // dynamic_cast and typeid on objects made by the other image.
  Base *Object = dllMakeNodeInt();
  assert(dynamic_cast<Node<int> *>(Object) != nullptr);
  assert(dynamic_cast<Node<char> *>(Object) == nullptr);
  assert(typeid(*Object) == typeid(Node<int>));
  Node<int> Local;
  assert(dllIsNodeInt(Local));
  delete Object;

  // Catch matching against the other image's descriptor.
  bool Caught = false;
  try {
    dllThrowNodeChar();
  } catch (const Node<int> &) {
    assert(false && "wrong handler");
  } catch (const Node<char> &) {
    Caught = true;
  }
  assert(Caught);
  return 0;
}
#endif
