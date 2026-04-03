// Test environment variable access and manipulation.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: env TEST_VAR1=hello TEST_VAR2=world %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// UCRT environ accessor.
extern char ***__p__environ(void);

int main(int argc, char **argv, char **envp) {
  // CHECK: Environment test
  printf("Environment test\n");

  // envp should be non-null.
  // CHECK: envp valid = 1
  printf("envp valid = %d\n", envp != NULL);

  // __p__environ() should return pointer to environ.
  char **environ_ptr = *__p__environ();
  // CHECK: environ valid = 1
  printf("environ valid = %d\n", environ_ptr != NULL);

  // envp and *__p__environ() should be equivalent.
  // CHECK: envp equals environ = 1
  printf("envp equals environ = %d\n", envp == environ_ptr);

  // Count environment variables.
  int count = 0;
  for (char **e = envp; *e != NULL; ++e) {
    count++;
  }
  // CHECK: env count > 0 = 1
  printf("env count > 0 = %d\n", count > 0);

  // Look for test variables.
  const char *test1 = getenv("TEST_VAR1");
  const char *test2 = getenv("TEST_VAR2");

  // CHECK: TEST_VAR1 = hello
  printf("TEST_VAR1 = %s\n", test1 ? test1 : "(null)");

  // CHECK: TEST_VAR2 = world
  printf("TEST_VAR2 = %s\n", test2 ? test2 : "(null)");

  // Test setenv equivalent (putenv).
  int putenv_result = _putenv("MY_NEW_VAR=testing123");
  // CHECK: putenv succeeded = 1
  printf("putenv succeeded = %d\n", putenv_result == 0);

  const char *new_var = getenv("MY_NEW_VAR");
  // CHECK: MY_NEW_VAR = testing123
  printf("MY_NEW_VAR = %s\n", new_var ? new_var : "(null)");

  // Test PATH-like variable exists.
  const char *path = getenv("PATH");
  // CHECK: PATH exists = 1
  printf("PATH exists = %d\n", path != NULL);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
