// A program that replaces operator new and operator delete replaces them for
// every image, as it does on ELF: the shared C++ runtime holds the extern
// template instantiations of basic_string, so a string that crosses the
// boundary is released by whichever allocator took it.
//
// RUN: %clangxx_crt -DBUILD_DLL -shared %s -o %t.dll -Wl,-implib:%t.import.lib
// RUN: %clangxx_crt_main %s %t.import.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// The library keeps its own operators to itself and carries the records that
// say they may be superseded; the program publishes the ones it replaced, and
// the table the library searches is built from that.
// RUN: llvm-readobj --coff-exports --section-headers %t.dll | FileCheck %s --check-prefix=LIBRARY
// RUN: llvm-readobj --coff-exports --section-headers %t.exe | FileCheck %s --check-prefix=PROGRAM
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>
#include <string>

#define API __attribute__((visibility("default")))

API std::string makeString(const char *Text);
API void takeString(std::string Value);
API void *allocateInLibrary(size_t Size);
API void releaseInLibrary(void *Memory);

#ifdef BUILD_DLL

std::string makeString(const char *Text) { return std::string(Text); }
void takeString(std::string Value) { (void)Value; }
void *allocateInLibrary(size_t Size) { return ::operator new(Size); }
void releaseInLibrary(void *Memory) { ::operator delete(Memory); }

#else

// The program's allocator. Every block it hands out carries a header, so a
// block released by the wrong allocator is caught rather than silently
// corrupting a heap.
namespace {
constexpr unsigned long long Cookie = 0x4F50455241544F52ULL;
int Live;
int Allocations;
} // namespace

void *operator new(size_t Size) {
  auto *Header = static_cast<unsigned long long *>(malloc(Size + 16));
  if (Header == nullptr)
    abort();
  *Header = Cookie;
  ++Allocations;
  ++Live;
  return Header + 2;
}

void operator delete(void *Memory) noexcept {
  if (Memory == nullptr)
    return;
  auto *Header = static_cast<unsigned long long *>(Memory) - 2;
  if (*Header != Cookie) {
    printf("released by the wrong allocator\n");
    abort();
  }
  *Header = 0;
  --Live;
  free(Header);
}

void operator delete(void *Memory, size_t) noexcept { ::operator delete(Memory); }

int main() {
  // A block the library allocates comes from the program's operator new, and
  // the program can release it.
  void *FromLibrary = allocateInLibrary(64);
  int AfterLibraryAllocation = Allocations;
  ::operator delete(FromLibrary);

  // CHECK: library allocation: program 1
  printf("library allocation: program %d\n", AfterLibraryAllocation > 0);

  // And the other way round.
  releaseInLibrary(::operator new(64));

  // A string the library builds is long enough to allocate, and the program
  // destroys it.
  {
    std::string Crossed =
        makeString("a string long enough that it does not fit inline at all");
    // CHECK-NEXT: string crossed: 1
    printf("string crossed: %d\n", Crossed.size() > 32);
  }

  // A string the program builds is destroyed inside the library.
  takeString(std::string("another string long enough to need an allocation"));

  // CHECK-NEXT: balanced: 1
  printf("balanced: %d\n", Live == 0);
  return 0;
}

#endif

// LIBRARY: Name: .wkintp
// LIBRARY-NOT: Name: .wkpub
// LIBRARY-NOT: Name: _Zn
// LIBRARY-NOT: Name: _Zd

// The three the program defines, and nothing else of the twenty.
// PROGRAM-DAG: Name: _Znwy
// PROGRAM-DAG: Name: _ZdlPv
// PROGRAM-DAG: Name: _ZdlPvy
// PROGRAM-DAG: Name: .wkpub
