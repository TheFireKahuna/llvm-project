// DEFINE: %{cxx} = %clang_wincrt -std=c++17 -fno-exceptions
// DEFINE: %{wide} = -Wl,-delayload:shape.dll
// RUN: rm -rf %t && mkdir -p %t/r1 %t/r2 %t/mixed %t/bin
// RUN: %{cxx} -c -DSHAPE %s -o %t/shape.o
// RUN: %{cxx} -c -DSHAPE -DOTHER %s -o %t/shape2.o
// RUN: %{cxx} -c -DPLUGIN %s -o %t/plugin.o
// RUN: %{cxx} -c -DPLUGIN -DHOOK %s -o %t/hook.o
// RUN: %{cxx} -DHOST %s -o %t/bin/host.exe

// RUN: %{cxx} -shared %t/shape.o %{wide} -o %t/shape.dll \
// RUN:   -Wl,-lldjointscan:%t/shape.jscan
// RUN: %{cxx} -shared %t/plugin.o %t/shape.dll.lib %{wide} -o %t/plugin.dll \
// RUN:   -Wl,-lldjointscan:%t/plugin.jscan
// RUN: %{cxx} -shared %t/hook.o %t/shape.dll.lib %{wide} -o %t/hook.dll \
// RUN:   -Wl,-lldjointscan:%t/hook.jscan
// RUN: %lld_link -lldjointmerge -out:%t/f1 %t/shape.jscan %t/plugin.jscan \
// RUN:   %t/hook.jscan
// RUN: %{cxx} -shared %t/shape.o %{wide} -o %t/r1/shape.dll \
// RUN:   -Wl,-lldjoint:%t/f1/shape.dll.jfacts
// RUN: %{cxx} -shared %t/plugin.o %t/shape.dll.lib %{wide} \
// RUN:   -o %t/r1/plugin.dll -Wl,-lldjoint:%t/f1/plugin.dll.jfacts
// RUN: %{cxx} -shared %t/hook.o %t/shape.dll.lib %{wide} -o %t/r1/hook.dll \
// RUN:   -Wl,-lldjoint:%t/f1/hook.dll.jfacts

// RUN: %{cxx} -shared %t/shape2.o %{wide} -o %t/shape.dll \
// RUN:   -Wl,-lldjointscan:%t/shape.jscan
// RUN: %lld_link -lldjointmerge -out:%t/f2 %t/shape.jscan %t/plugin.jscan \
// RUN:   %t/hook.jscan
// RUN: %{cxx} -shared %t/shape2.o %{wide} -o %t/r2/shape.dll \
// RUN:   -Wl,-lldjoint:%t/f2/shape.dll.jfacts
// RUN: cp %t/r1/plugin.dll %t/r1/hook.dll %t/r2/shape.dll %t/mixed/

// RUN: cd %t/r1 && %run %t/bin/host.exe %t/r1/plugin.dll \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: cd %t/mixed && %run %t/bin/host.exe %t/mixed/plugin.dll \
// RUN:   | FileCheck --check-prefix=MIXED %s
// RUN: cd %t/mixed && %run %t/bin/host.exe %t/mixed/hook.dll \
// RUN:   | FileCheck --check-prefixes=HOOK,MIXED %s

// A plugin linked jointly with shape.dll, which it delay-loads, imports
// shape.dll's identity, which covers the layout of the class whose layout the
// plugin uses, in place of a witness of it. Code never calls the identity, so
// the delay-load helper resolves it first, as it resolves a witness: a
// shape.dll from another merge, here one that also exports another class,
// lacks the identity, so the first call fails as a missing import fails,
// through the failure hook and then the exception, and never reaches its code.

// SAME: area 12, height 4
// HOOK: failure hook: shape.dll lacks __llvm_private$
// MIXED: the first call failed: 0xC06D007F
// MIXED-NOT: area

#define API __attribute__((visibility("default")))

struct Shape {
  int w, h;
  API Shape(int w, int h);
  API int area() const;
};

#if defined(SHAPE)
Shape::Shape(int w, int h) : w(w), h(h) {}
int Shape::area() const { return w * h; }
#  ifdef OTHER
struct Other {
  int a, b;
  API int sum() const;
};
int Other::sum() const { return a + b; }
#  endif

#elif defined(PLUGIN)
#  include <stdio.h>
#  include <windows.h>
#  ifdef HOOK
#    include <delayimp.h>
static FARPROC WINAPI failureHook(unsigned Reason, DelayLoadInfo *Dli) {
  if (Reason == dliFailGetProc && Dli->dlp.fImportByName)
    printf("failure hook: %s lacks %.15s\n", Dli->szDll, Dli->dlp.szProcName);
  fflush(stdout);
  return nullptr;
}
const PfnDliHook __pfnDliFailureHook2 = failureHook;
#  endif
extern "C" API void measure() {
  Shape s(3, 4);
  printf("area %d, height %d\n", s.area(), s.h);
}

#elif defined(HOST)
#  include <stdio.h>
#  include <windows.h>
int main(int argc, char **argv) {
  // The loader takes the plugin's directory for its dependencies only from a
  // path with backslashes.
  for (char *c = argv[1]; *c; ++c)
    if (*c == '/')
      *c = '\\';
  HMODULE m = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!m) {
    printf("not loaded, error %lu\n", GetLastError());
    return 1;
  }
  auto measure = reinterpret_cast<void (*)()>(
      reinterpret_cast<void *>(GetProcAddress(m, "measure")));
  __try {
    measure();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    printf("the first call failed: 0x%08lX\n", GetExceptionCode());
  }
  return 0;
}
#endif
