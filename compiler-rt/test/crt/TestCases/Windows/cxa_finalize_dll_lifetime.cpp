// A callback selected for process cleanup must survive concurrent FreeLibrary.
// RUN: %clangxx_crt_dll -O2 -UNDEBUG -DBUILD_DLL %s -o %t.dll
// RUN: %clangxx_crt_main -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe %t.dll
// REQUIRES: windows, crt

#include <assert.h>
#include <windows.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void __cxa_finalize(void *);
extern "C" void *__dso_handle;
struct State {
  HANDLE entered, resume;
  LONG calls;
};

#ifdef BUILD_DLL
static void destroy(void *object) {
  auto &state = *static_cast<State *>(object);
  assert(SetEvent(state.entered));
  assert(WaitForSingleObject(state.resume, 5000) == WAIT_OBJECT_0);
  assert(InterlockedIncrement(&state.calls) == 1);
}
extern "C" __declspec(dllexport) int install(State *state) {
  return __cxa_atexit(destroy, state, __dso_handle);
}
#else
static DWORD WINAPI finalize(void *) {
  __cxa_finalize(nullptr);
  return 0;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  State state{CreateEventW(nullptr, TRUE, FALSE, nullptr),
              CreateEventW(nullptr, TRUE, FALSE, nullptr), 0};
  assert(state.entered && state.resume);
  HMODULE module = LoadLibraryA(argv[1]);
  assert(module);
  auto install =
      reinterpret_cast<int (*)(State *)>(GetProcAddress(module, "install"));
  assert(install && install(&state) == 0);
  HANDLE thread = CreateThread(nullptr, 0, finalize, nullptr, 0, nullptr);
  assert(thread);
  assert(WaitForSingleObject(state.entered, 5000) == WAIT_OBJECT_0);
  assert(FreeLibrary(module));
  assert(GetModuleHandleA(argv[1]));
  assert(SetEvent(state.resume));
  assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  DWORD code;
  assert(GetExitCodeThread(thread, &code) && code == 0);
  assert(state.calls == 1);
  assert(!GetModuleHandleA(argv[1]));
  assert(CloseHandle(thread));
  assert(CloseHandle(state.entered));
  assert(CloseHandle(state.resume));
}
#endif
