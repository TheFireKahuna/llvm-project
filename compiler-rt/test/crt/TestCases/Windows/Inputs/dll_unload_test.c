// Test harness for DLL load/unload with static destructor verification.

#include <stdio.h>

typedef void *HMODULE;
typedef int (*DLL_FUNC)(void);

#define __stdcall __attribute__((ms_abi))

__declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char *);
__declspec(dllimport) int __stdcall FreeLibrary(HMODULE);
__declspec(dllimport) void *__stdcall GetProcAddress(HMODULE, const char *);

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: %s <dll_path>\n", argv[0]);
    return 1;
  }

  const char *dll_path = argv[1];
  printf("=== Loading DLL: %s ===\n", dll_path);

  HMODULE hDll = LoadLibraryA(dll_path);
  if (!hDll) {
    printf("Failed to load DLL\n");
    return 1;
  }
  printf("DLL loaded at %p\n", hDll);

  DLL_FUNC func = (DLL_FUNC)GetProcAddress(hDll, "dll_function");
  if (func) {
    printf("Calling dll_function...\n");
    int result = func();
    printf("dll_function returned: %d\n", result);
  } else {
    printf("dll_function not found\n");
  }

  printf("=== Unloading DLL ===\n");
  if (!FreeLibrary(hDll)) {
    printf("Failed to unload DLL\n");
    return 1;
  }
  printf("DLL unloaded\n");

  printf("=== Test complete ===\n");
  return 0;
}
