// RUN: %clang_wincrt %s -o %t.exe
// RUN: %python -c "import subprocess, sys; sys.exit(subprocess.call([sys.argv[1], 'return']) != 42)" %t.exe
// RUN: %python -c "import subprocess, sys; sys.exit(subprocess.call([sys.argv[1], 'exit']) != 43)" %t.exe

// The value main returns, or the one it passes to exit, is the exit code.

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "exit"))
    exit(43);
  return 42;
}
