// Registrations from EXEs and DLLs must use the same thread cleanup owner.
// RUN: %clangxx_crt_dll -std=c++17 -O2 -DBUILD_DLL %s -o %t.one.dll
// RUN: %clangxx_crt_dll -std=c++17 -O2 -DBUILD_DLL %s -o %t.two.dll
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe %t.one.dll %t.two.dll worker | FileCheck %s --check-prefix=EXIT
// RUN: %run %t.exe %t.one.dll %t.two.dll main | FileCheck %s --check-prefix=EXIT
// RUN: %run %t.exe %t.one.dll %t.two.dll unload | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t.exe %t.one.dll %t.two.dll release | FileCheck %s --check-prefix=RELEASE
// REQUIRES: windows, crt

#include <stdio.h>

struct Object {
  int id;
  ~Object() { printf("destroy %d\n", id); }
};

#ifdef BUILD_DLL
extern "C" __declspec(dllexport) void touch(int id) {
  thread_local Object object{id};
}
#else
#  include <assert.h>
#  include <thread>
#  include <windows.h>

int main(int argc, char **argv) {
  assert(argc == 4);
  HMODULE one = LoadLibraryA(argv[1]);
  HMODULE two = LoadLibraryA(argv[2]);
  assert(one && two);
  auto touchOne = reinterpret_cast<void (*)(int)>(GetProcAddress(one, "touch"));
  auto touchTwo = reinterpret_cast<void (*)(int)>(GetProcAddress(two, "touch"));
  assert(touchOne && touchTwo);
  auto construct = [&] {
    thread_local Object first{1};
    touchOne(2);
    thread_local Object second{3};
    touchTwo(4);
  };
  if (argv[3][0] == 'r') {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE finish = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(ready && finish);
    std::thread worker([&] {
      construct();
      assert(SetEvent(ready));
      assert(WaitForSingleObject(finish, 5000) == WAIT_OBJECT_0);
    });
    assert(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    assert(FreeLibrary(two));
    assert(FreeLibrary(one));
    assert(GetModuleHandleA(argv[1]) == one);
    assert(GetModuleHandleA(argv[2]) == two);
    printf("released handles\n");
    assert(SetEvent(finish));
    worker.join();
    assert(GetModuleHandleA(argv[1]) == nullptr);
    assert(GetModuleHandleA(argv[2]) == nullptr);
    printf("unloaded after join\n");
    CloseHandle(ready);
    CloseHandle(finish);
    return 0;
  }
  if (argv[3][0] == 'w')
    std::thread(construct).join();
  else
    construct();
  if (argv[3][0] == 'u') {
    assert(FreeLibrary(two));
    assert(FreeLibrary(one));
    assert(GetModuleHandleA(argv[1]) == one);
    assert(GetModuleHandleA(argv[2]) == two);
    printf("released handles\n");
  }
}
#endif

// EXIT: destroy 4
// EXIT-NEXT: destroy 3
// EXIT-NEXT: destroy 2
// EXIT-NEXT: destroy 1
// EXIT-NOT: destroy
// UNLOAD: released handles
// UNLOAD-NEXT: destroy 4
// UNLOAD-NEXT: destroy 3
// UNLOAD-NEXT: destroy 2
// UNLOAD-NEXT: destroy 1
// UNLOAD-NOT: destroy
// RELEASE: released handles
// RELEASE-NEXT: destroy 4
// RELEASE-NEXT: destroy 3
// RELEASE-NEXT: destroy 2
// RELEASE-NEXT: destroy 1
// RELEASE-NEXT: unloaded after join
// RELEASE-NOT: destroy
