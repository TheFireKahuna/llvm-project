// DEFINE: %{cxx} = %clang_wincrt -std=c++17 -fno-exceptions
// RUN: rm -rf %t && mkdir -p %t/same %t/wide
// RUN: %{cxx} -shared -DSHAPE %s -o %t/shape.dll
// RUN: %{cxx} -shared -DPLUGIN %s %t/shape.dll.lib -o %t/plugin.dll
// RUN: %{cxx} -DHOST %s -o %t/host.exe
// RUN: cp %t/plugin.dll %t/same/ && cp %t/plugin.dll %t/wide/
// RUN: %{cxx} -shared -DSHAPE -DOTHER_CODE %s -o %t/same/shape.dll
// RUN: %{cxx} -shared -DSHAPE -DWIDE %s -o %t/wide/shape.dll
// RUN: %run %t/host.exe %t/plugin.dll | FileCheck --check-prefix=FIRST %s
// RUN: %run %t/host.exe %t/same/plugin.dll | FileCheck --check-prefix=SAME %s
// RUN: %run %t/host.exe %t/wide/plugin.dll | FileCheck --check-prefix=WIDE %s
// RUN: not %{cxx} -shared -DPLUGIN %s %t/wide/shape.dll.lib \
// RUN:   -o %t/relinked.dll 2>&1 | FileCheck --check-prefix=LINK %s

// The plugin uses the layout of Shape, which shape.dll defines, and imports
// the witness of the layout it was compiled with. A shape.dll rebuilt with
// other code but the same layout still loads under it; one rebuilt with
// another layout does not, since it exports another witness, and a link
// against that DLL's import library fails. Each load is a process of its own,
// since a process holds one shape.dll.

// FIRST: plugin.dll: area 12, height 4
// SAME: plugin.dll: area 13, height 4
// WIDE: plugin.dll: not loaded, error 127
// LINK: error: ABI mismatch for 'Shape': {{.*}} was compiled with a different layout than shape.dll exports (needed because {{.*}} uses Shape::Shape(int, int))

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
Shape::Shape(int w, int h) : w(w), h(h) {}
#  ifdef OTHER_CODE
int Shape::area() const { return w * h + 1; }
#  else
int Shape::area() const { return w * h; }
#  endif

#elif defined(PLUGIN)
#  include <stdio.h>
extern "C" API void measure(const char *name) {
  Shape s(3, 4);
  printf("%s: area %d, height %d\n", name, s.area(), s.h);
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
    printf("%s: not loaded, error %lu\n", argv[1], GetLastError());
    return 0;
  }
  auto measure = reinterpret_cast<void (*)(const char *)>(
      reinterpret_cast<void *>(GetProcAddress(m, "measure")));
  measure(argv[1]);
  return 0;
}
#endif
