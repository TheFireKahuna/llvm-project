// RUN: %clang %s -o %t.exe
// RUN: %exit_status %t.exe one "two words" | FileCheck %s --check-prefixes=CHECK,ARGV
// RUN: %clang -DWIDE %s -o %t-wide.exe
// RUN: %exit_status %t-wide.exe one "two words" | FileCheck %s --check-prefixes=CHECK,ARGV
// RUN: %clang -mwindows -DGUI %s -o %t-gui.exe
// RUN: llvm-readobj --file-headers %t-gui.exe | FileCheck %s --check-prefix=HEADER
// RUN: %exit_status %t-gui.exe one "two words" | FileCheck %s --check-prefixes=CHECK,LINE
// RUN: %clang -mwindows -DGUI -DWIDE %s -o %t-wgui.exe
// RUN: %exit_status %t-wgui.exe one "two words" | FileCheck %s --check-prefixes=CHECK,LINE
// RUN: %clang -mwindows %s -o %t-gui-main.exe
// RUN: llvm-readobj --file-headers %t-gui-main.exe | FileCheck %s --check-prefix=HEADER
// RUN: %exit_status %t-gui-main.exe one "two words" | FileCheck %s --check-prefixes=CHECK,ARGV

// The linker picks the entry point from the program's main function: main,
// wmain, WinMain or wWinMain. Each gets the Universal CRT's arguments and
// environment, and the value it returns is the exit code. A GUI program that
// defines main rather than WinMain runs its main as a GUI application.

#include <corecrt_startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#ifdef WIDE
#  define CHAR wchar_t
#  define FORMAT "%ls\n"
#  define MAIN wmain
#  define WINMAIN wWinMain
#  define ARGV __wargv
#  define ENVIRON _wenviron
#else
#  define CHAR char
#  define FORMAT "%s\n"
#  define MAIN main
#  define WINMAIN WinMain
#  define ARGV __argv
#  define ENVIRON _environ
#endif

#ifdef GUI
int WINAPI WINMAIN(HINSTANCE Instance, HINSTANCE Previous, CHAR *CommandLine,
                   int Show) {
  if (Instance != GetModuleHandleW(NULL) || Previous || Show < 0 ||
      _query_app_type() != _crt_gui_app)
    return 1;
  printf(FORMAT, CommandLine);
  return 3;
}
#else
int MAIN(int argc, CHAR **argv, CHAR **envp) {
  if (argc != __argc || argv != ARGV || envp != ENVIRON || argv[argc])
    return 1;
  for (int I = 1; I < argc; ++I)
    printf(FORMAT, argv[I]);
  return argc;
}
#endif

// HEADER: Subsystem: IMAGE_SUBSYSTEM_WINDOWS_GUI

// ARGV:       one
// ARGV-NEXT:  two words
// LINE:       one "two words"
// CHECK-NEXT: exit code 3
