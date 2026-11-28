// RUN: %clang -fno-exceptions -shared -DRT %s -o %t-rt.dll
// RUN: %clang -fno-exceptions -shared -DPLUGIN %s %t-rt.dll.lib -o %t-plugin.dll
// RUN: %clang -fno-exceptions -shared -DFOREIGN --target=x86_64-pc-windows-msvc -fuse-ld=lld \
// RUN:   -nostdlib -Wl,-noentry %s %t-rt.dll.lib -o %t-foreign.dll
// RUN: %clang -fno-exceptions %s %t-rt.dll.lib %t-foreign.lib -o %t.exe
// RUN: %clang -fno-exceptions -DPLAIN %s %t-rt.dll.lib %t-foreign.lib -o %t-plain.exe
// RUN: %run %t.exe %t-plugin.dll | FileCheck %s
// RUN: %run %t-plain.exe %t-plugin.dll | FileCheck --check-prefix=PLAIN %s
// RUN: %clang -fno-exceptions -shared -DOFFER %s %t-rt.dll.lib -o %t-offer.dll
// RUN: %clang -fno-exceptions -DRT -DSTATIC %s %t-offer.dll.lib -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck --check-prefix=STATIC %s
// RUN: %clang -fno-exceptions -flto -shared -DLTO %s -o %t-lto.dll
// RUN: %clang -fno-exceptions -DLTO -DUSER %s %t-lto.dll.lib -o %t-lto.exe
// RUN: %run %t-lto.exe | FileCheck --check-prefix=LTO %s

// REQUIRES: target={{x86_64-.*}}

// An executable that replaces operator new and the unsized operator delete
// replaces them for every image. Its own calls reach its replacements
// directly, sized operator delete's and operator new[]'s too, which forward
// to them. A DLL's calls read its import's entry, which the DLL's fill writes
// once from the executable's table with the replacement: so do those of a
// plugin that the executable loads later. The runtime's own calls of a
// default reach the default's own entry, which reads the default's word, and
// a caller of another producer reaches the default's checked entry, which
// transfers to the replacement. Every image observes one address for each
// default: the replacement's where the executable replaces it, and the
// runtime's own where it forwards, and a local name of a default the
// default's own, as on ELF. A program that replaces nothing writes no
// word in either DLL. One whose table or anchor has been changed does not
// start, and nor does one whose slot and anchor both name a place that no
// call may reach, which a DLL's call cell would otherwise reach unchecked.
// An executable that defines the defaults itself, as a static runtime does,
// takes a DLL's offer for its own calls and address-takes too.
// A DLL that LTO compiles keeps a default that it neither exports nor offers
// for the words of its calls and address-takes, which the executable's
// replacement then reaches.

// CHECK:      exe: new 1, delete 1, defaults 0
// CHECK-NEXT: runtime: new 2, delete 2, defaults 0
// CHECK-NEXT: plugin: new 4, delete 4, defaults 0
// CHECK-NEXT: foreign: new 5, delete 4, defaults 0
// CHECK-NEXT: address: new 1 1, new[] 1 1, the runtime's 0, own 1
// CHECK-NEXT: written: runtime 1, plugin 1
// CHECK-NEXT: erased: c0000142
// CHECK-NEXT: version: c0000142
// CHECK-NEXT: count: c0000142
// CHECK-NEXT: dropped: c0000142
// CHECK-NEXT: moved: c0000142
// CHECK-NEXT: stray: c0000142
// CHECK-NEXT: stub: c0000142
// CHECK-NEXT: offers: c0000142
// CHECK-NEXT: forged: c0000142

// PLAIN:      exe: new 0, delete 0, defaults 4
// PLAIN-NEXT: runtime: new 0, delete 0, defaults 6
// PLAIN-NEXT: plugin: new 0, delete 0, defaults 12
// PLAIN-NEXT: foreign: new 0, delete 0, defaults 13
// PLAIN-NEXT: address: new 1 1, new[] 1 1, the runtime's 1, own 1
// PLAIN-NEXT: written: runtime 0, plugin 0

// STATIC: static: offered 4, defaults 0, address 1

// LTO: lto: replaced 2, defaults 0

#include <windows.h>

typedef __SIZE_TYPE__ size_t;

#if defined(FOREIGN)
// A caller that another producer made reaches the runtime's own entry.
extern "C" __declspec(dllimport) void *rt_new(size_t) __asm__("_Znwy");
extern "C" __declspec(dllexport) void *foreign_new(size_t n) {
  return rt_new(n);
}
#elif defined(RT)
static int defaults;
extern "C" __declspec(dllexport) int rt_defaults() { return defaults; }

[[gnu::weak, clang::overridable_default(0)]] __declspec(dllexport) void *
operator new(size_t n) {
  ++defaults;
  return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}
[[gnu::weak, clang::overridable_default(1, 0)]] __declspec(dllexport) void *
operator new[](size_t n) {
  ++defaults;
  return ::operator new(n);
}
[[gnu::weak, clang::overridable_default(2)]] __declspec(dllexport) void
operator delete(void *p) noexcept {
  ++defaults;
  HeapFree(GetProcessHeap(), 0, p);
}
[[gnu::weak, clang::overridable_default(3, 2, true)]] __declspec(dllexport) void
operator delete(void *p, size_t n) noexcept {
  ++defaults;
  ::operator delete(p);
}

extern "C" __declspec(dllexport) void rt_run() {
  ::operator delete(::operator new(8));
}
#  if defined(STATIC)
#    include <stdio.h>
extern "C" __declspec(dllimport) int offer_count();
extern "C" __declspec(dllimport) void *offer_address();
int main() {
  ::operator delete(::operator new[](8), 8);
  rt_run();
  printf("static: offered %d, defaults %d, address %d\n", offer_count(),
         defaults,
         (void *)static_cast<void *(*)(size_t)>(&::operator new) ==
             offer_address());
  return 0;
}
#  endif
__asm__("own_new = _Znwy");
extern "C" [[gnu::visibility("hidden")]] void *own_new(size_t);
extern "C" __declspec(dllexport) void rt_address(void **out) {
  out[0] = (void *)static_cast<void *(*)(size_t)>(&::operator new);
  out[1] = (void *)static_cast<void *(*)(size_t)>(&::operator new[]);
  out[2] = (void *)&own_new;
}
#elif defined(LTO) && !defined(USER)
static int defaults;
[[gnu::weak, clang::overridable_default(0)]] void *operator new(size_t n) {
  ++defaults;
  return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}
extern "C" __declspec(dllexport) int lto_run(void *(**address)(size_t)) {
  *address = static_cast<void *(*)(size_t)>(&::operator new);
  HeapFree(GetProcessHeap(), 0, ::operator new(8));
  return defaults;
}
#elif defined(LTO)
#  include <stdio.h>
static int replaced;
void *operator new(size_t n) {
  ++replaced;
  return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}
extern "C" __declspec(dllimport) int lto_run(void *(**)(size_t));
int main() {
  void *(*address)(size_t);
  int defaults = lto_run(&address);
  HeapFree(GetProcessHeap(), 0, address(8));
  printf("lto: replaced %d, defaults %d\n", replaced, defaults);
  return 0;
}
#elif defined(OFFER)
static int offered;
extern "C" __declspec(dllexport) int offer_count() { return offered; }
__declspec(dllexport) void *operator new(size_t n) {
  ++offered;
  return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}
__declspec(dllexport) void operator delete(void *p) noexcept {
  ++offered;
  HeapFree(GetProcessHeap(), 0, p);
}
extern "C" __declspec(dllexport) void *offer_address() {
  return (void *)static_cast<void *(*)(size_t)>(&::operator new);
}
#elif defined(PLUGIN)
extern "C" __declspec(dllexport) void plugin_run() {
  ::operator delete(::operator new[](8), 8);
  ::operator delete(::operator new(8));
}
extern "C" __declspec(dllexport) void plugin_address(void **out) {
  out[0] = (void *)static_cast<void *(*)(size_t)>(&::operator new);
  out[1] = (void *)static_cast<void *(*)(size_t)>(&::operator new[]);
}
#else
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" __declspec(dllimport) int rt_defaults();
extern "C" __declspec(dllimport) void rt_run();
extern "C" __declspec(dllimport) void rt_address(void **);
extern "C" __declspec(dllimport) void *foreign_new(size_t);

static int replaced_new, replaced_del;
#ifndef PLAIN
void *operator new(size_t n) {
  ++replaced_new;
  return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}
void operator delete(void *p) noexcept {
  ++replaced_del;
  HeapFree(GetProcessHeap(), 0, p);
}
#endif

// The anchor in the file of an executable, from its section .ovra.
static char *anchor(char *file) {
  IMAGE_NT_HEADERS *nt =
      (IMAGE_NT_HEADERS *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
  IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i)
    if (!memcmp(section[i].Name, ".ovra", 6))
      return file + section[i].PointerToRawData;
  return nullptr;
}

// The table: slot k's tag and RVA at 0x40 + 8 * k, the header at 0x80 with
// its magic, format and count, and the offer table's RVA at 0x38; the anchor:
// a header of 0x18 bytes, then each slot's address. Slot 0 is operator new's.
struct Tamper {
  const char *name;
  void (*change)(char *file, char *anchor);
};
static const Tamper tampers[] = {
    {"erased", [](char *f, char *) { *(uint32_t *)(f + 0x80) = 0; }},
    {"version", [](char *f, char *) { ++*(uint16_t *)(f + 0x84); }},
    {"count", [](char *f, char *) { --*(uint16_t *)(f + 0x86); }},
    {"dropped", [](char *f, char *) { memset(f + 0x40, 0, 8); }},
    {"moved", [](char *f, char *) { *(uint32_t *)(f + 0x44) += 16; }},
    {"stray",
     [](char *f, char *) {
       *(uint32_t *)(f + 0x40 + 8 * 5) = 0xC5E1A7F3 ^ 5;
       *(uint32_t *)(f + 0x44 + 8 * 5) = *(uint32_t *)(f + 0x44);
     }},
    {"stub",
     [](char *f, char *) {
       static const char dos[] = "\x0e\x1f\xba\x0e\x00\xb4\x09\xcd\x21\xb8"
                                 "\x01\x4c\xcd\x21This program cannot be "
                                 "run in DOS mode.\r\r\n$";
       memcpy(f + 0x40, dos, sizeof(dos));
       memset(f + 0x40 + sizeof(dos), 0, 0x98 - 0x40 - sizeof(dos));
     }},
    {"offers", [](char *f, char *) { *(uint32_t *)(f + 0x38) += 8; }},
    {"forged",
     [](char *f, char *a) {
       *(uint32_t *)(f + 0x44) += 2;
       *(uint64_t *)(a + 0x18) += 2;
     }},
};

static void report(const char *what) {
  printf("%s: new %d, delete %d, defaults %d\n", what, replaced_new,
         replaced_del, rt_defaults());
}

// The word that the sealed section begins with, which the fill sets once it
// has written.
static int written(HMODULE module) {
  char *base = (char *)module;
  IMAGE_NT_HEADERS *nt =
      (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i)
    if (!memcmp(section[i].Name, ".sealed", 8))
      return *(const int *)(base + section[i].VirtualAddress);
  return -1;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ::operator delete(::operator new[](8), 8);
  report("exe");
  rt_run();
  report("runtime");
  HMODULE plugin = LoadLibraryA(argv[1]);
  ((void (*)())GetProcAddress(plugin, "plugin_run"))();
  report("plugin");
  HeapFree(GetProcessHeap(), 0, foreign_new(8));
  report("foreign");
  void *exe[2] = {(void *)static_cast<void *(*)(size_t)>(&::operator new),
                  (void *)static_cast<void *(*)(size_t)>(&::operator new[])};
  void *rt[3], *plug[2];
  rt_address(rt);
  ((void (*)(void **))GetProcAddress(plugin, "plugin_address"))(plug);
  HMODULE runtime;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&rt_run, &runtime);
  void *own = (void *)GetProcAddress(runtime, "_Znwy");
  printf("address: new %d %d, new[] %d %d, the runtime's %d, own %d\n",
         exe[0] == rt[0], exe[0] == plug[0], exe[1] == rt[1],
         exe[1] == plug[1], exe[0] == own, rt[2] == own);
  printf("written: runtime %d, plugin %d\n", written(runtime),
         written(plugin));
#ifndef PLAIN
  // Copies of the executable, each changed as one way of tampering with its
  // table in the header page or with the anchor, which the runtime's fill
  // finds apart or naming a place that is no function, and so fails.
  for (const Tamper &T : tampers) {
    FILE *F = fopen(argv[0], "rb");
    fseek(F, 0, SEEK_END);
    long size = ftell(F);
    char *file = (char *)malloc(size);
    fseek(F, 0, SEEK_SET);
    fread(file, 1, size, F);
    fclose(F);
    T.change(file, anchor(file));
    char copy[MAX_PATH];
    snprintf(copy, sizeof(copy), "%s-%s.exe", argv[0], T.name);
    F = fopen(copy, "wb");
    fwrite(file, 1, size, F);
    fclose(F);
    free(file);
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION child;
    DWORD status = 0;
    if (CreateProcessA(copy, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup,
                       &child)) {
      WaitForSingleObject(child.hProcess, INFINITE);
      GetExitCodeProcess(child.hProcess, &status);
    }
    printf("%s: %lx\n", T.name, status);
  }
#endif
  return 0;
}
#endif
