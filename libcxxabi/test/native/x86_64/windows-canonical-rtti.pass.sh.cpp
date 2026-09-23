// REQUIRES: target={{x86_64-.*-windows-itanium}}
// UNSUPPORTED: no-exceptions, c++03
// RUN: %{cxx} %{flags} %{compile_flags} -O2 -DTEST_OWNER %s %{link_flags} -shared -Wl,/noentry -Xlinker /export:_ZTI6Shared,DATA -Xlinker /export:_ZTS6Shared,DATA -Xlinker /export:_ZTIP6Shared,DATA -Xlinker /export:_ZTSP6Shared,DATA -Xlinker /export:_ZTSP6Opaque,DATA -Xlinker /export:_ZTS6Opaque,DATA -Wl,/implib:%t.owner.lib -o %t.owner.dll
// RUN: %{cxx} %{flags} %{compile_flags} -O2 %s %{link_flags} %t.owner.lib -shared -Wl,/noentry -o %t.consumer.dll
// RUN: %{cxx} %{flags} %{compile_flags} -DTEST_HOST %s %{link_flags} -o %t.exe
// RUN: %{exec} %t.exe %t.consumer.dll
// RUN: %{cxx} %{flags} %{compile_flags} -O2 -flto %s %{link_flags} %t.owner.lib -shared -Wl,/noentry -o %t.consumer.dll
// RUN: %{exec} %t.exe %t.consumer.dll
// RUN: %{cxx} %{flags} %{compile_flags} -O2 -flto=thin %s %{link_flags} %t.owner.lib -shared -Wl,/noentry -o %t.consumer.dll
// RUN: %{exec} %t.exe %t.consumer.dll

#ifdef TEST_HOST
#include <windows.h>
#include <cassert>

int main(int argc, char **argv) {
  assert(argc == 2);
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
  policy.ProhibitDynamicCode = 1;
  assert(SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &policy,
                                    sizeof(policy)));
  // Both participants load after ACG is enabled. The ordinary native loader
  // must bind all RTTI, vtable and LSDA fields without executable-page writes.
  HMODULE consumer = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  assert(consumer);
  auto check = reinterpret_cast<int (*)()>(GetProcAddress(consumer, "check"));
  assert(check && check() == 0);
  auto descriptor = reinterpret_cast<const void *(*)()>(
      GetProcAddress(consumer, "descriptor"));
  assert(descriptor);
  const void *rtti = descriptor();
  const void *name = static_cast<const void *const *>(rtti)[1];
  MEMORY_BASIC_INFORMATION info;
  assert(VirtualQuery(rtti, &info, sizeof(info)) == sizeof(info));
  assert(info.Protect == PAGE_READONLY);
  assert(VirtualQuery(name, &info, sizeof(info)) == sizeof(info));
  assert(info.Protect == PAGE_READONLY);
}
#else
#include <typeinfo>

#ifdef TEST_OWNER
#define OWNER __declspec(dllexport)
#else
#define OWNER __declspec(dllimport)
#endif

struct OWNER Base { virtual ~Base(); };
struct Shared : Base { int value = 42; };
struct Other : Base {};
struct Opaque;
#ifdef TEST_OWNER
struct Opaque {};
#endif
extern "C" {
OWNER Base *make_shared();
OWNER const std::type_info *provider_type();
OWNER const std::type_info *provider_pointer();
OWNER const std::type_info *provider_opaque();
OWNER void throw_shared();
}

#ifdef TEST_OWNER
Base::~Base() = default;
extern "C" Base *make_shared() {
  static Shared value;
  return &value;
}
extern "C" const std::type_info *provider_type() { return &typeid(Shared); }
extern "C" const std::type_info *provider_pointer() { return &typeid(Shared *); }
extern "C" const std::type_info *provider_opaque() { return &typeid(Opaque *); }
extern "C" void throw_shared() { throw Shared(); }
#else
static_assert(sizeof(std::type_info) == 2 * sizeof(void *));
extern "C" __declspec(dllexport) const void *descriptor() {
  return &typeid(Shared);
}
extern "C" __declspec(dllexport) int check() {
  const std::type_info &a = typeid(Shared), &b = *provider_type();
  if (&a != &b || a.name() != b.name() || a != b ||
      a.hash_code() != b.hash_code() || a.before(b) || b.before(a))
    return 1;
  if (&typeid(Shared *) != provider_pointer())
    return 2;
  // Incomplete descriptors are deliberately distinct, but generic type_info
  // equality must still agree through the canonical name.
  const std::type_info &incomplete = typeid(Opaque *);
  const std::type_info &complete = *provider_opaque();
  if (&incomplete == &complete || incomplete != complete ||
      incomplete.name() != complete.name() ||
      incomplete.hash_code() != complete.hash_code())
    return 10;
  Base *object = make_shared();
  if (&typeid(*object) != &a)
    return 3;
  auto *shared = dynamic_cast<Shared *>(object);
  if (!shared || shared->value != 42 || dynamic_cast<Other *>(object))
    return 4;
  Shared local;
  if (&typeid(static_cast<Base &>(local)) != &a)
    return 5;
  try {
    throw_shared();
    return 6;
  } catch (const Shared &exception) {
    if (exception.value != 42)
      return 7;
  }
  try {
    throw_shared();
    return 8;
  } catch (const Base &exception) {
    if (!dynamic_cast<const Shared *>(&exception))
      return 9;
  }
  return 0;
}
#endif
#endif
