// DLL loader helper for dllmain.c test.

#include <stdio.h>

typedef void *HMODULE;
typedef int (*FARPROC)(void);

#define __stdcall __attribute__((ms_abi))

__declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char *);
__declspec(dllimport) int __stdcall FreeLibrary(HMODULE);
__declspec(dllimport) FARPROC __stdcall GetProcAddress(HMODULE, const char *);

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: %s <dll_path>\n", argv[0]);
    return 1;
  }

  const char *dll_path = argv[1];
  printf("Loading DLL: %s\n", dll_path);

  HMODULE hDll = LoadLibraryA(dll_path);
  if (!hDll) {
    printf("Failed to load DLL\n");
    return 1;
  }

  printf("DLL loaded successfully\n");

  FARPROC func = GetProcAddress(hDll, "test_function");
  if (func) {
    int result = func();
    printf("test_function returned: %d\n", result);
  } else {
    printf("test_function not found\n");
  }

  printf("Unloading DLL\n");
  FreeLibrary(hDll);

  printf("Done\n");
  return 0;
}
