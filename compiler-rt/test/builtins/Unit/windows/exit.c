// RUN: %clang -shared -DDLL %s -o %t.dll
// RUN: %clang -shared -DFOREIGN -Wl,-entry:foreignEntry %s -o %t-foreign.dll
// RUN: %clang %s %t.dll.lib -o %t.exe
// DEFINE: %{exit} = FileCheck %s --check-prefixes=CHECK,EXIT,DRAIN,MODULES
// RUN: %run %t.exe return | %{exit},MAIN
// RUN: %run %t.exe exit | %{exit},MAIN
// RUN: %run %t.exe dll-exit | %{exit},MAIN
// RUN: %run %t.exe thread | %{exit},THREAD
// RUN: %exit_status %t.exe reentry | %{exit},MAIN,REENTRY,STATUS -DCODE=7
// RUN: %exit_status %t.exe second-thread | %{exit},MAIN,BLOCKED,STATUS -DCODE=0
// RUN: %exit_status %t.exe thread_local-exit | %{exit},TLS-EXIT,MAIN,STATUS -DCODE=8
// RUN: %exit_status %t.exe handler-quick_exit \
// RUN:   | FileCheck %s --check-prefixes=CHECK,MAIN,EXIT,HANDLER-QUICK,QUICK,MODULES,STATUS -DCODE=6
// RUN: %run %t.exe quick_exit | FileCheck %s --check-prefixes=CHECK,QUICK,MODULES
// RUN: %run %t.exe dll-quick_exit | FileCheck %s --check-prefixes=CHECK,QUICK,MODULES
// RUN: %run %t.exe foreign %t-foreign.dll | FileCheck %s --check-prefixes=CHECK,QUICK
// RUN: %run %t.exe _exit | FileCheck %s --check-prefixes=CHECK,MODULES
// RUN: %run %t.exe _Exit | FileCheck %s --check-prefixes=CHECK,MODULES
// RUN: %clang -static %s %t.dll.lib -o %t-static.exe
// RUN: %run %t-static.exe return | %{exit},MAIN
// RUN: %exit_status %t-static.exe reentry | %{exit},MAIN,REENTRY,STATUS -DCODE=7
// RUN: %run %t-static.exe quick_exit | FileCheck %s --check-prefixes=CHECK,QUICK,MODULES
// RUN: %clang -DHOST -Wl,-entry:hostEntry -Wl,-subsystem:console %s %t.dll.lib -o %t-host.exe
// RUN: %run %t-host.exe | FileCheck %s --check-prefix=HOST
// RUN: llvm-readobj --coff-imports %t.exe %t.dll | FileCheck %s --check-prefix=IMPORT \
// RUN:   --implicit-check-not='Symbol: exit ' --implicit-check-not='Symbol: _exit ' \
// RUN:   --implicit-check-not='Symbol: _Exit ' --implicit-check-not='Symbol: quick_exit '
// RUN: %clang -DPLAIN %s -o %t-plain.exe
// RUN: llvm-readobj --file-headers %t-plain.exe | FileCheck %s --check-prefix=PLAIN
// RUN: %clang -static -DPLAIN %s -o %t-plain-static.exe
// RUN: llvm-readobj --file-headers --coff-imports %t-plain-static.exe \
// RUN:   | FileCheck %s --check-prefix=PLAIN --implicit-check-not=wincrt_dynamic

// [CHECK] A thread's thread-local destructors run in reverse order when it
// exits, and one registered while they run runs next.
// [EXIT] [DRAIN] Returning from main and exit, from main, another thread or a DLL, run
// the exiting thread's thread-locals, then a function another runtime put in
// the Universal CRT's atexit table, then the atexit, _onexit and __cxa_atexit
// registrations of every image in one reverse order, one registered during
// the drain next, then .CRT$XP and .CRT$XT; DLL detach follows, and output is
// flushed after it.
// [REENTRY] exit from a registered function continues the drain: the entries
// left, of every image, run once, newest first, one registered during the
// nested drain among them, and the process ends with the last status given.
// [BLOCKED] exit from another thread during the drain does not return and
// runs nothing; the draining thread's status ends the process. [TLS-EXIT]
// exit from a thread-local destructor still runs the drain. [HANDLER-QUICK]
// quick_exit from a registered function runs the quick list and ends the
// process.
// [QUICK] quick_exit, from any image, runs every image's at_quick_exit
// registrations in reverse order and no other; so does the Universal CRT's,
// called by a DLL with no wincrt start-up. _exit and _Exit run neither.
// [MODULES] None of these loads kernel.appcore.dll or msvcrt.dll, which the
// Universal CRT's exit loads; -static keeps every rule.
// [HOST] Under an executable with no wincrt start-up, quick_exit is the
// Universal CRT's, which runs the executable's registrations with the DLL's.
// [IMPORT] Images reach wincrt's functions and import none of the Universal
// CRT's. [PLAIN] A program that registers no thread-local destructor links no
// thread registry and so has no TLS directory; under -static it imports no
// runtime DLL.

// CHECK:         thread 3
// CHECK-NEXT:    thread 2
// CHECK-NEXT:    registered while they run
// CHECK-NEXT:    thread 1
// TLS-EXIT-NEXT: a thread_local calls exit
// MAIN-NEXT:     main thread_local
// THREAD-NEXT:   second thread's thread_local
// EXIT-NEXT:     another runtime's atexit function
// EXIT-NEXT:     atexit 4
// EXIT-NEXT:     registered during exit
// REENTRY-NEXT:  a handler calls exit
// REENTRY-NEXT:  registered by that handler
// REENTRY-NEXT:  registered during the nested drain
// BLOCKED-NEXT:  second thread's exit blocked 1
// HANDLER-QUICK-NEXT: a handler calls quick_exit
// DRAIN-NEXT:    __cxa_atexit 3
// DRAIN-NEXT:    dll atexit
// DRAIN-NEXT:    dll __cxa_atexit
// DRAIN-NEXT:    _onexit 2
// DRAIN-NEXT:    atexit 1
// DRAIN-NEXT:    registered by a constructor
// DRAIN-NEXT:    pre-terminator
// DRAIN-NEXT:    terminator
// QUICK-NEXT:    at_quick_exit 1
// QUICK-NEXT:    at_quick_exit dll
// QUICK-NEXT:    at_quick_exit 2
// MODULES-NEXT:  kernel.appcore.dll 0, msvcrt.dll 0
// STATUS-NEXT:   exit code [[CODE]]
// MODULES-NOT:   {{.}}

// HOST:      at_quick_exit dll
// HOST-NEXT: at_quick_exit host

// IMPORT-COUNT-2: Name: clang_rt.wincrt_dynamic.dll
// PLAIN:          TLSTableSize: 0x0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int __cxa_atexit(void (*)(void *), void *, void *);
int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
int _crt_atexit(void (*)(void));
extern void *__dso_handle;

static void say(void *Text) { printf("%s\n", (const char *)Text); }
#define SAY(Name, Text)                                                        \
  static void Name(void) { say(Text); }

#if defined(DLL)

SAY(dllAtexit, "dll atexit")
SAY(dllQuick, "at_quick_exit dll")

__declspec(dllexport) void dllRegister(void) {
  __cxa_atexit(say, "dll __cxa_atexit", &__dso_handle);
  atexit(dllAtexit);
  at_quick_exit(dllQuick);
}
__declspec(dllexport) void dllExit(int Quick) { Quick ? quick_exit(0) : exit(0); }

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH)
    printf("kernel.appcore.dll %d, msvcrt.dll %d\n",
           GetModuleHandleW(L"kernel.appcore.dll") != NULL,
           GetModuleHandleW(L"msvcrt.dll") != NULL);
  return TRUE;
}

#elif defined(FOREIGN)

__declspec(dllexport) void foreignQuickExit(void) { quick_exit(0); }
BOOL WINAPI foreignEntry(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  return TRUE;
}

#elif defined(HOST)

#  include <corecrt_startup.h>
__declspec(dllimport) void dllRegister(void);
__declspec(dllimport) void dllExit(int);
SAY(hostQuick, "at_quick_exit host")

// Registers as a Visual C++ executable's start-up does.
int hostEntry(void) {
  _crt_at_quick_exit(hostQuick);
  dllRegister();
  dllExit(1);
  return 1;
}

#elif defined(PLAIN)

static void nothing(void) {}
int main(void) { return atexit(nothing); }

#else

__declspec(dllimport) void dllRegister(void);
__declspec(dllimport) void dllExit(int);

SAY(first, "atexit 1")
SAY(late, "registered during exit")
SAY(fromConstructor, "registered by a constructor")
SAY(preterminate, "pre-terminator")
SAY(terminate, "terminator")
SAY(otherRuntime, "another runtime's atexit function")
SAY(quick1, "at_quick_exit 1")
SAY(quick2, "at_quick_exit 2")
static int second(void) { return say("_onexit 2"), 0; }
static void fourth(void) { say("atexit 4"), atexit(late); }
SAY(nestedLate, "registered during the nested drain")
static void registrar(void) { say("registered by that handler"), atexit(nestedLate); }
static void reenter(void) { say("a handler calls exit"), atexit(registrar), exit(7); }
static void quickFromHandler(void) { say("a handler calls quick_exit"), quick_exit(6); }
static void exitFromThreadLocal(void *Text) { say(Text), exit(8); }
static DWORD WINAPI exitAgain(LPVOID Parameter) { exit(9); }
static void blockExit(void) {
  HANDLE Thread = CreateThread(NULL, 0, exitAgain, NULL, 0, NULL);
  printf("second thread's exit blocked %d\n", WaitForSingleObject(Thread, 500) == WAIT_TIMEOUT);
}
static void construct(void) { atexit(fromConstructor); }

__attribute__((used, section(".CRT$XCU"))) static void (*const C)(void) = construct;
__attribute__((used, section(".CRT$XPU"))) static void (*const P)(void) = preterminate;
__attribute__((used, section(".CRT$XTU"))) static void (*const T)(void) = terminate;

static void registerDuringDrain(void *Text) {
  say(Text);
  __cxa_thread_atexit_impl(say, "registered while they run", &__dso_handle);
}
static DWORD WINAPI threadLocals(LPVOID Parameter) {
  __cxa_thread_atexit_impl(say, "thread 1", &__dso_handle);
  __cxa_thread_atexit_impl(registerDuringDrain, "thread 2", &__dso_handle);
  __cxa_thread_atexit_impl(say, "thread 3", &__dso_handle);
  return 0;
}
static DWORD WINAPI exitThread(LPVOID Parameter) {
  __cxa_thread_atexit_impl(say, "second thread's thread_local", &__dso_handle);
  exit(0);
}

int main(int argc, char **argv) {
  const char *Mode = argv[1];
  WaitForSingleObject(CreateThread(NULL, 0, threadLocals, NULL, 0, NULL), INFINITE);
  atexit(first);
  _onexit(second);
  at_quick_exit(quick2);
  dllRegister();
  __cxa_atexit(say, "__cxa_atexit 3", &__dso_handle);
  at_quick_exit(quick1);
  __cxa_thread_atexit_impl(say, "main thread_local", &__dso_handle);
  if (!strcmp(Mode, "thread_local-exit"))
    __cxa_thread_atexit_impl(exitFromThreadLocal, "a thread_local calls exit", &__dso_handle);
  _crt_atexit(otherRuntime);
  if (!strcmp(Mode, "reentry"))
    atexit(reenter);
  if (!strcmp(Mode, "second-thread"))
    atexit(blockExit);
  if (!strcmp(Mode, "handler-quick_exit"))
    atexit(quickFromHandler);
  atexit(fourth);
  if (!strcmp(Mode, "exit"))
    exit(0);
  if (!strcmp(Mode, "_exit"))
    _exit(0);
  if (!strcmp(Mode, "_Exit"))
    _Exit(0);
  if (!strcmp(Mode, "quick_exit"))
    quick_exit(0);
  if (!strncmp(Mode, "dll-", 4))
    dllExit(!strcmp(Mode, "dll-quick_exit"));
  if (!strcmp(Mode, "foreign"))
    ((void (*)(void))GetProcAddress(LoadLibraryA(argv[2]), "foreignQuickExit"))();
  if (!strcmp(Mode, "thread")) {
    CreateThread(NULL, 0, exitThread, NULL, 0, NULL);
    Sleep(INFINITE);
  }
  return 0;
}

#endif
