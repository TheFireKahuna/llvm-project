// RUN: %clang -O2 -fstack-protector-strong -shared -DDLL %s -o %t.dll
// RUN: %clang --target=x86_64-pc-windows-msvc -fstack-protector-strong -c -DMSVC %s -o %t-msvc.obj
// RUN: %clang -O2 -fstack-protector-strong %s %t-msvc.obj %t.dll.lib -o %t.exe
// RUN: %clang -O2 -fstack-protector-strong -static %s %t-msvc.obj %t.dll.lib -o %t-static.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %run %t-static.exe | FileCheck %s
// RUN: not --crash %run %t.exe overflow 2>&1 | FileCheck %s --allow-empty --check-prefix=OVERFLOW
// RUN: not --crash %run %t.exe dll-overflow 2>&1 | FileCheck %s --allow-empty --check-prefix=OVERFLOW
// RUN: not --crash %run %t-static.exe overflow 2>&1 | FileCheck %s --allow-empty --check-prefix=OVERFLOW
// REQUIRES: x86_64-target-arch

// A protected function reads its image's own guard, __stack_chk_guard, which
// is __security_cookie: one read-only word that the loader writes with a
// random 48-bit value as it maps the image, before any of its code runs. So a
// frame of the executable that a DLL's initializer enters, and parks on a
// fiber until main, returns cleanly, under -static too, and an object that
// MSVC's conventions built initializes and checks the same word under its
// MSVC names. An overflow of a protected buffer
// fails fast, with no output.

// CHECK:        exe guard written
// CHECK-NEXT:   dll guard written
// CHECK-NEXT:   msvc cookie checked
// CHECK-NEXT:   parked frame returned
// CHECK-NEXT:   protected calls returned
// OVERFLOW-NOT: {{written|checked|returned}}

#ifdef MSVC
// Built for MSVC, which names the word __security_cookie and has its start-up
// call __security_init_cookie.
extern unsigned long long __security_cookie;
void __security_init_cookie(void);
void __security_check_cookie(unsigned long long);
void use(char *);

int msvc_protected(void) {
  char Buffer[16];
  __security_init_cookie();
  use(Buffer);
  __security_check_cookie(__security_cookie);
  return Buffer[0];
}
#else
#  include <stdint.h>
#  include <stdio.h>
#  include <string.h>
#  include <windows.h>

extern const uintptr_t __stack_chk_guard, __security_cookie;

// Whether the image's two names are one word, an address the compiler takes
// for two variables' and cannot compare itself.
static int oneWord(void) {
  const uintptr_t *Guard = &__stack_chk_guard;
  __asm__("" : "+r"(Guard));
  return Guard == &__security_cookie;
}

// Writes N bytes into a 16-byte buffer of a protected frame.
static __attribute__((noinline)) int fill(char *Buffer, size_t N) {
  memset(Buffer, 'x', N);
  return Buffer[N - 1];
}

// Whether an image's guard holds the loader's value, in a read-only page.
static int written(const uintptr_t *Guard) {
  MEMORY_BASIC_INFORMATION Info;
  return *Guard && *Guard != 0x2B992DDFA232 && !(*Guard >> 48) &&
         VirtualQuery(Guard, &Info, sizeof(Info)) &&
         Info.Protect == PAGE_READONLY;
}

#  ifdef DLL
__declspec(dllexport) int dll_protected(size_t N) {
  char Buffer[16];
  return fill(Buffer, N);
}

__declspec(dllexport) const uintptr_t *dll_guard(void) {
  return oneWord() ? &__stack_chk_guard : NULL;
}

// The executable exports exe_attach, which this initializer calls before any
// initializer of the executable has run.
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  void (*Attach)(void) =
      (void (*)(void))GetProcAddress(GetModuleHandleW(NULL), "exe_attach");
  if (Reason == DLL_PROCESS_ATTACH && Attach)
    Attach();
  return TRUE;
}
#  else
__declspec(dllimport) int dll_protected(size_t N);
__declspec(dllimport) const uintptr_t *dll_guard(void);
int msvc_protected(void);

void use(char *Buffer) { Buffer[0] = 1; }

static __attribute__((noinline)) int protected_call(size_t N) {
  char Buffer[16];
  return fill(Buffer, N);
}

static void *Main, *Held;
static volatile int Done;

// A protected frame that stays on the fiber's stack, holding its guard, from
// the DLL's initializer until main resumes it.
static __attribute__((noinline)) void hold(void) {
  char Buffer[16];
  fill(Buffer, sizeof(Buffer));
  SwitchToFiber(Main);
}

static void WINAPI fiber(void *Unused) {
  hold();
  Done = 1;
  SwitchToFiber(Main);
}

__declspec(dllexport) void exe_attach(void) {
  Main = ConvertThreadToFiber(NULL);
  Held = CreateFiber(0, fiber, NULL);
  SwitchToFiber(Held);
}

int main(int Argc, char **Argv) {
  if (Argc > 1 && !strcmp(Argv[1], "overflow"))
    return protected_call(64);
  if (Argc > 1 && !strcmp(Argv[1], "dll-overflow"))
    return dll_protected(64);
  if (oneWord() && written(&__stack_chk_guard))
    puts("exe guard written");
  if (dll_guard() && written(dll_guard()))
    puts("dll guard written");
  if (msvc_protected() == 1)
    puts("msvc cookie checked");
  if (Held)
    SwitchToFiber(Held);
  if (Done)
    puts("parked frame returned");
  protected_call(16);
  dll_protected(16);
  puts("protected calls returned");
  return 0;
}
#  endif
#endif
