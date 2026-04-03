// Test basic C++ exception handling with Itanium ABI.
//
// RUN: %clang_crt_main -std=c++17 -fexceptions %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

struct Resource {
  int id;
  Resource(int i) : id(i) {
    printf("Resource(%d) acquired\n", id);
  }
  ~Resource() {
    printf("Resource(%d) released\n", id);
  }
};

void throws_int(int value) {
  Resource r(100);
  printf("About to throw int %d\n", value);
  throw value;
  printf("This should not print\n");
}

void throws_string(const char *msg) {
  Resource r(200);
  printf("About to throw string '%s'\n", msg);
  throw msg;
  printf("This should not print\n");
}

class CustomException {
  int code_;
public:
  explicit CustomException(int c) : code_(c) {
    printf("CustomException(%d) constructed\n", c);
  }
  CustomException(const CustomException &other) : code_(other.code_) {
    printf("CustomException(%d) copied\n", code_);
  }
  ~CustomException() {
    printf("CustomException(%d) destructed\n", code_);
  }
  int code() const { return code_; }
};

void throws_custom(int code) {
  Resource r(300);
  printf("About to throw CustomException(%d)\n", code);
  throw CustomException(code);
}

int main() {
  // CHECK: Exception handling test
  printf("Exception handling test\n");

  // Test 1: Catch int.
  // CHECK: Test 1: int exception
  printf("Test 1: int exception\n");
  try {
    Resource outer(1);
    throws_int(42);
    printf("This should not print\n");
  } catch (int e) {
    // CHECK: Resource(1) acquired
    // CHECK: Resource(100) acquired
    // CHECK: About to throw int 42
    // CHECK: Resource(100) released
    // CHECK: Resource(1) released
    // CHECK: Caught int: 42
    printf("Caught int: %d\n", e);
  }

  // Test 2: Catch const char*.
  // CHECK: Test 2: string exception
  printf("Test 2: string exception\n");
  try {
    throws_string("error message");
  } catch (const char *e) {
    // CHECK: Resource(200) acquired
    // CHECK: About to throw string 'error message'
    // CHECK: Resource(200) released
    // CHECK: Caught string: error message
    printf("Caught string: %s\n", e);
  }

  // Test 3: Catch custom exception by reference.
  // CHECK: Test 3: custom exception
  printf("Test 3: custom exception\n");
  try {
    throws_custom(999);
  } catch (const CustomException &e) {
    // CHECK: Resource(300) acquired
    // CHECK: About to throw CustomException(999)
    // CHECK: CustomException(999) constructed
    // CHECK: Resource(300) released
    // CHECK: Caught CustomException with code: 999
    printf("Caught CustomException with code: %d\n", e.code());
  }
  // CHECK: CustomException(999) destructed

  // Test 4: Catch-all.
  // CHECK: Test 4: catch-all
  printf("Test 4: catch-all\n");
  try {
    throw 3.14159;
  } catch (...) {
    // CHECK: Caught unknown exception type
    printf("Caught unknown exception type\n");
  }

  // Test 5: Rethrow.
  // CHECK: Test 5: rethrow
  printf("Test 5: rethrow\n");
  try {
    try {
      throw 123;
    } catch (int e) {
      // CHECK: Inner catch: 123, rethrowing
      printf("Inner catch: %d, rethrowing\n", e);
      throw;
    }
  } catch (int e) {
    // CHECK: Outer catch: 123
    printf("Outer catch: %d\n", e);
  }

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
