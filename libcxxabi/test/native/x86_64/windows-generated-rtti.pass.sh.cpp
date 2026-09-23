// REQUIRES: target={{x86_64-.*-windows-itanium}}
// UNSUPPORTED: no-exceptions, c++03
// RUN: %{cxx} %{flags} %{compile_flags} -O2 %s %{link_flags} -shared -Wl,/noentry -o %t.right.dll
// RUN: %{cxx} %{flags} %{compile_flags} -O2 -DTEST_COMPLETE %s %{link_flags} -shared -Wl,/noentry -o %t.left.dll
// RUN: %{cxx} %{flags} %{compile_flags} -DTEST_HOST %s %{link_flags} -o %t.exe
// RUN: %{exec} %t.exe %t.left.dll %t.right.dll
// RUN: %{cxx} %{flags} %{compile_flags} -O2 -flto %s %{link_flags} -shared -Wl,/noentry -o %t.right.dll
// RUN: %{exec} %t.exe %t.left.dll %t.right.dll
// RUN: %{cxx} %{flags} %{compile_flags} -O2 -flto=thin %s %{link_flags} -shared -Wl,/noentry -o %t.right.dll
// RUN: %{exec} %t.exe %t.left.dll %t.right.dll

#include <typeinfo>

struct Base {
  virtual ~Base() = default;
};
struct Left : virtual Base {};
struct Right : virtual Base {};
struct Shared : Left, Right {
  int value = 42;
};
struct Opaque;
#ifdef TEST_COMPLETE
struct Opaque {};
#endif

#ifdef TEST_HOST
#  include <windows.h>
#  include <cassert>

int main(int argc, char** argv) {
  assert(argc == 3);
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
  policy.ProhibitDynamicCode = 1;
  assert(SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &policy, sizeof(policy)));
  HMODULE b = LoadLibraryExA(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  HMODULE a = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  assert(a && b);
  auto object = reinterpret_cast<Base* (*)()>(GetProcAddress(a, "object"));
  auto descriptor = reinterpret_cast<const std::type_info* (*)()>(GetProcAddress(a, "descriptor"));
  auto thrower = reinterpret_cast<void (*)()>(GetProcAddress(a, "thrower"));
  auto check = reinterpret_cast<int (*)(Base*, const std::type_info*, void (*)())>(GetProcAddress(b, "check"));
  assert(object && descriptor && thrower && check);
  assert(check(object(), descriptor(), thrower) == 0);
  auto complete = reinterpret_cast<const std::type_info* (*)()>(GetProcAddress(a, "opaque"));
  auto incomplete = reinterpret_cast<const std::type_info* (*)()>(GetProcAddress(b, "opaque"));
  assert(complete && incomplete);
  assert(complete() != incomplete());
  assert(*complete() == *incomplete());
  assert(complete()->name() == incomplete()->name());
  MEMORY_BASIC_INFORMATION info;
  const std::type_info* rtti = descriptor();
  assert(VirtualQuery(rtti, &info, sizeof(info)) == sizeof(info));
  assert(info.Protect == PAGE_READONLY);
  assert(VirtualQuery(rtti->name(), &info, sizeof(info)) == sizeof(info));
  assert(info.Protect == PAGE_READONLY);
  assert(FreeLibrary(b));
  assert(FreeLibrary(a));
}
#else
static_assert(sizeof(std::type_info) == 2 * sizeof(void*));
extern "C" __declspec(dllexport) Base* object() {
  static Shared value;
  return &value;
}
extern "C" __declspec(dllexport) const std::type_info* descriptor() { return &typeid(Shared); }
extern "C" __declspec(dllexport) void thrower() { throw Shared(); }
extern "C" __declspec(dllexport) const std::type_info* opaque() { return &typeid(Opaque*); }
extern "C" __declspec(dllexport) int check(Base* object, const std::type_info* other, void (*thrower)()) {
  const std::type_info& local = typeid(Shared);
  if (&local != other || local.name() != other->name() || local.hash_code() != other->hash_code() ||
      local.before(*other) || other->before(local) || &typeid(*object) != other)
    return 1;
  auto* shared = dynamic_cast<Shared*>(object);
  if (!shared || shared->value != 42 || !dynamic_cast<Right*>(object))
    return 2;
  try {
    thrower();
    return 3;
  } catch (const Shared& exception) {
    if (exception.value != 42)
      return 4;
  }
  try {
    thrower();
    return 5;
  } catch (const Base& exception) {
    if (!dynamic_cast<const Shared*>(&exception))
      return 6;
  }
  return 0;
}
#endif
