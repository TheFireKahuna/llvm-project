// RUN: %clang_wincrt -mwindows %s -o %t.exe
// RUN: llvm-readobj --file-headers %t.exe | FileCheck %s --check-prefix=HEADER
// RUN: %run %t.exe one "two words" | FileCheck %s

// The linker picks wWinMainCRTStartup for a wide GUI program.

#include <stdio.h>
#include <windows.h>

int WINAPI wWinMain(HINSTANCE Instance, HINSTANCE Previous, LPWSTR CommandLine,
                    int Show) {
  if (Instance != GetModuleHandleW(NULL) || Previous || Show < 0)
    return 1;
  printf("%ls\n", CommandLine);
  return 0;
}

// HEADER: Subsystem: IMAGE_SUBSYSTEM_WINDOWS_GUI
// CHECK:  one "two words"
