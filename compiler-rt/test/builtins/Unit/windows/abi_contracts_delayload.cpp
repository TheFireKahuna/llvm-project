// DEFINE: %{cxx} = %clang_wincrt -std=c++17 -fno-exceptions
// RUN: rm -rf %t && mkdir -p %t/same %t/wide %t/bin
// RUN: %{cxx} -shared -DSHAPE %s -o %t/shape.dll
// RUN: %{cxx} -shared -DPLUGIN %s %t/shape.dll.lib -Wl,-delayload:shape.dll \
// RUN:   -o %t/loader.dll
// RUN: %{cxx} -shared -DPLUGIN %s %t/shape.dll.lib -Wl,-delayload:shape.dll \
// RUN:   -Wl,-delay:unload -o %t/unload.dll
// RUN: %{cxx} -shared -DPLUGIN -DHOOK %s %t/shape.dll.lib \
// RUN:   -Wl,-delayload:shape.dll -o %t/hook.dll
// RUN: %{cxx} -shared -DPLUGIN -DHOOK -DSUBSTITUTE %s %t/shape.dll.lib \
// RUN:   -Wl,-delayload:shape.dll -o %t/substitute.dll
// RUN: %{cxx} -DHOST %s -o %t/bin/host.exe
// RUN: cp %t/loader.dll %t/unload.dll %t/hook.dll %t/same/
// RUN: cp %t/loader.dll %t/unload.dll %t/hook.dll %t/substitute.dll %t/wide/
// RUN: %{cxx} -shared -DSHAPE -DOTHER_CODE %s -o %t/same/shape.dll
// RUN: %{cxx} -shared -DSHAPE -DWIDE %s -o %t/wide/shape.dll
// RUN: cd %t/same && %run %t/bin/host.exe %t/same/loader.dll \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: cd %t/same && %run %t/bin/host.exe %t/same/unload.dll \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: cd %t/same && %run %t/bin/host.exe %t/same/hook.dll \
// RUN:   | FileCheck --check-prefix=SAME %s
// RUN: cd %t/wide && %run %t/bin/host.exe %t/wide/loader.dll \
// RUN:   | FileCheck --check-prefix=WIDE %s
// RUN: cd %t/wide && %run %t/bin/host.exe %t/wide/unload.dll \
// RUN:   | FileCheck --check-prefix=WIDE %s
// RUN: cd %t/wide && %run %t/bin/host.exe %t/wide/hook.dll \
// RUN:   | FileCheck --check-prefixes=HOOK,WIDE %s
// RUN: cd %t/wide && %run %t/bin/host.exe %t/wide/substitute.dll \
// RUN:   | FileCheck --check-prefixes=HOOK,WIDE %s

// A plugin that delay-loads shape.dll, whose class's layout it uses, binds
// shape.dll's witness of that layout when it first calls into it. Code never
// calls a witness, so the delay-load helper resolves the witness first: a
// shape.dll rebuilt with other code but the same layout serves the call; one
// rebuilt with another layout lacks the witness, so the first call fails as a
// missing import fails, through the failure hook and then the exception, and
// never reaches the rebuilt code. This holds through the loader, when the
// image resolves its own imports under /delay:unload, and with a hook, even
// one that answers every missing import with a substitute. The
// host is apart from shape.dll, which a delay load finds in the current
// directory.

// SAME: area 13, height 4
// HOOK: failure hook: shape.dll lacks __abiw$
// WIDE: the first call failed: 0xC06D007F
// WIDE-NOT: {{area|substitute}}

#define API __attribute__((visibility("default")))

struct Shape {
  int w;
#ifdef WIDE
  int extra;
#endif
  int h;
  API Shape(int w, int h);
  API int area() const;
};

#if defined(SHAPE)
#  include <stdio.h>
Shape::Shape(int w, int h) : w(w), h(h) {}
#  ifdef OTHER_CODE
int Shape::area() const { return w * h + 1; }
#  else
int Shape::area() const { return w * h; }
#  endif

#elif defined(PLUGIN)
#  include <stdio.h>
#  include <windows.h>
#  ifdef HOOK
#    include <delayimp.h>
#    ifdef SUBSTITUTE
static void substitute() { printf("substitute\n"); }
#    endif
static FARPROC WINAPI failureHook(unsigned Reason, DelayLoadInfo *Dli) {
  if (Reason == dliFailGetProc && Dli->dlp.fImportByName)
    printf("failure hook: %s lacks %.7s\n", Dli->szDll, Dli->dlp.szProcName);
  fflush(stdout);
#    ifdef SUBSTITUTE
  return reinterpret_cast<FARPROC>(&substitute);
#    else
  return nullptr;
#    endif
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
