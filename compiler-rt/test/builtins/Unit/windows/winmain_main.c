// RUN: %clang_wincrt -mwindows %s -o %t.exe
// RUN: llvm-readobj --file-headers %t.exe | FileCheck %s --check-prefix=HEADER
// RUN: %run %t.exe one "two words" | FileCheck %s
// RUN: %clang_wincrt -mwindows -DWINMAIN %s -o %t-winmain.exe
// RUN: %run %t-winmain.exe one | FileCheck %s --check-prefix=WINMAIN

// A GUI program that defines main rather than WinMain runs its main as a GUI
// application, with its arguments; one that defines both runs WinMain.

#include <corecrt_startup.h>
#include <stdio.h>
#include <windows.h>

int main(int argc, char **argv) {
  printf("main %d %s %s, GUI %d\n", argc, argv[1], argv[2],
         _query_app_type() == _crt_gui_app);
  return 0;
}

#ifdef WINMAIN
int WINAPI WinMain(HINSTANCE Instance, HINSTANCE Previous, LPSTR CommandLine,
                   int Show) {
  printf("WinMain %s\n", CommandLine);
  return 0;
}
#endif

// HEADER: Subsystem: IMAGE_SUBSYSTEM_WINDOWS_GUI
// CHECK: main 3 one two words, GUI 1
// WINMAIN: WinMain one
