// DEFINE: %{link} = %clang %s %t.dll.lib -Wl,/delayload:%basename_t.tmp.dll
// RUN: %clang -shared -DDLL -DMISSING %s -o %t.dll
// RUN: %{link} -mguard=none -o %t.exe
// RUN: %{link} -o %t-protected.exe
// RUN: %{link} -DWHOLE_SECTION -o %t-whole.exe
// RUN: %{link} -mguard=none -DNOTIFY_HOOK -o %t-hook.exe
// RUN: %{link} -mguard=none -Xlinker -subsystem:console,10.0 -DNOTIFY_HOOK -o %t-protected-hook.exe
// RUN: %{link} -mguard=none -DFAILURE_HOOK -o %t-failure-hook.exe
// RUN: %{link} -DFAILURE_HOOK -o %t-protected-failure-hook.exe
// RUN: %{link} -DFAILURE_HOOK -Wl,/delay:unload -o %t-unload-failure-hook.exe
// RUN: %{link} -mguard=none -Wl,/delay:unload -o %t-unload.exe
// RUN: %{link} -Wl,/delay:unload -o %t-protected-unload.exe
// The loader then searches only System32 for every DLL the executable loads,
// so it takes the termination registries from -static.
// RUN: %{link} -static -Wl,/dependentloadflag:0x800 -o %t-system32.exe
// A second DLL, which the address modes also delay-load.
// RUN: %clang -shared -DDLL -DSECOND %s -o %t-2.dll
// DEFINE: %{second} = %t-2.dll.lib -Wl,/delayload:%basename_t.tmp-2.dll
// RUN: %{link} %{second} -DADDRESS -o %t-address.exe
// RUN: %{link} %{second} -Wl,/nooptdidat -DADDRESS -o %t-address-kept.exe
// RUN: %clang -shared -DDLL -DMISSING %s -o %t-gone.dll
// RUN: %clang %s %t-gone.dll.lib -Wl,/delayload:%basename_t.tmp-gone.dll %{second} -DADDRESS -DGONE -o %t-address-gone.exe
// RUN: rm %t-gone.dll
// RUN: %run %t.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s
// RUN: %run %t-protected.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,PROTECTED,NARROW
// RUN: %run %t-whole.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,PROTECTED,WHOLE
// RUN: %run %t-hook.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,HOOK
// RUN: %run %t-protected-hook.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,PROTECTED,NARROW,HOOK
// RUN: %run %t-address.exe address %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=ADDRESS,OWN,GATE
// RUN: %run %t-address.exe keeps %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=KEEPS
// RUN: %run %t-address-kept.exe address %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=ADDRESS,KEPT
// RUN: %run %t-address-kept.exe keeps %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=KEEPS
// RUN: %run %t-address.exe keeps-read %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=KEEPS-READ
// RUN: %run %t-address-kept.exe keeps-read %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=KEEPS-READ
// RUN: %run %t-address-gone.exe address %basename_t.tmp-gone.dll %t.dll %t-gone.dll 2>&1 | FileCheck %s --check-prefixes=ADDRESS,GONE,OWN,GATE
// RUN: %run %t-address.exe load-all %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=LOAD-ALL-OWN
// The DLL loses the export the executables import as missing.
// RUN: %clang -shared -DDLL %s -o %t.dll
// RUN: %run %t.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,PROC
// RUN: %run %t-unload.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,PROC
// RUN: %run %t-failure-hook.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,FHOOK,LOADER
// RUN: %run %t-protected-failure-hook.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,FHOOK,LOADER
// RUN: %run %t-unload-failure-hook.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,FHOOK
// RUN: %run %t-system32.exe missing 2>&1 | FileCheck %s --check-prefix=MODULE
// RUN: %run %t.exe load-all %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=LOAD-ALL
// RUN: %run %t-unload.exe load-all %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=LOAD-ALL
// RUN: %run %t-unload-failure-hook.exe load-all %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=LOAD-ALL-HOOK
// RUN: %run %t-unload.exe unload %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t-protected-unload.exe unload %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t-protected.exe unload %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=NO-UNLOAD
// RUN: %run %t-address.exe address %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=ADDRESS,OWN,GATE

// [CHECK] A delayed import resolves on its first call, through the loader or,
// with a delayimp.h notify hook installed, in the image. [HOOK] The hook sees
// every stage of each import as it is called. [PROTECTED] Under Control Flow
// Guard, the default, or for a subsystem version above 6.3, since the helper's
// object defines __MSSafeDelayLoadIAT, the table is read-only before and after
// it is written. [NARROW] [WHOLE] A program's choice to make the whole section
// writable meanwhile is read as false when it makes none, and has no effect.
// [PROC] An import that cannot be resolved raises a structured exception
// carrying a DelayLoadInfo, rather than the helper returning a null address,
// and one never called never fails. [FHOOK] The failure hook is asked first;
// [LOADER] its answer is stored only when the image resolves the import
// itself. [MODULE] /dependentloadflag names the search path; restricted to
// System32, the DLL beside the executable is not found. [LOAD-ALL]
// __HrLoadAllImportsForDll reports a failure as a call does, on either path.
// [UNLOAD] __FUnloadDelayLoadedDLL2 restores the table and frees the library,
// so the next call loads it again, matching the name without regard to case
// and refusing a library the image does not delay-load. [NO-UNLOAD] Without
// the copy of the table /delay:unload makes, it refuses.
// [ADDRESS] Nothing loads a delay-loaded DLL until code reads a word holding
// the address of one of its functions, reads or takes the address of the
// data that holds such a word, or takes the address itself. [OWN] Each is
// behind the gate of every DLL whose words it may reach, which runs the stub
// that the linker gives the DLL: so the address is the DLL's own wherever it
// is observed, a pointer read first and compared later included, in a word
// of read-only data that the loader binds in place and leaves read-only, in
// one of writable data, which stays writable, and in code, while a word
// after such a one and a word the program stored keep their values, also
// where the DLL lacks another export; a table of two DLLs' functions, read
// directly and through its address, binds both. Each placement of the guard
// gives it, inline, as a call of a leaf for a lea and for a load whose flags
// stay live; any other read, a compare and an atomic exchange behind a lock
// written on a line of its own among them, is replayed behind the guard and
// sees the address code takes; an address 8 bytes into the function, which
// the DLL exports by a name of its own, is the DLL's too. [KEEPS] A guard
// keeps every other register, the flags and the vector state, through the
// stub and after, for one DLL and for two; [KEEPS-READ] so does a replay.
// [GATE] The DLLs' gates are bound then. [KEPT] An image whose table the
// loader does not protect keeps the thunks' addresses. [GONE] Where the DLL
// does not load, each guard returns with its words as they were, so that a
// pointer read then is the address code takes then, and binds them once the
// DLL is there.
// [LOAD-ALL-OWN] __HrLoadAllImportsForDll binds the words too.

// PROTECTED:      read-only before 1
// NARROW-NEXT:    whole section 0
// WHOLE-NEXT:     whole section 1
// HOOK:           hook start twice$tp
// HOOK-NEXT:      hook pre-load twice$tp
// HOOK-NEXT:      hook pre-getproc twice$tp
// HOOK-NEXT:      hook end twice$tp
// CHECK:          twice(21) = 42
// HOOK-NEXT:      hook start add$tp
// HOOK-NEXT:      hook pre-getproc add$tp
// HOOK-NEXT:      hook end add$tp
// CHECK:          add(1, 2) = 3
// HOOK-NEXT:      hook start missing
// HOOK-NEXT:      hook pre-getproc missing
// HOOK-NEXT:      hook end missing
// HOOK-NEXT:      hook start twice
// HOOK-NEXT:      hook pre-getproc twice
// HOOK-NEXT:      hook end twice
// CHECK-NEXT:     load all 0
// CHECK-NEXT:     load all of another not found
// PROTECTED-NEXT: read-only after 1

// MISSING:       twice(2) = 4
// MISSING-NEXT:  twice(2) = 4
// PROC-NEXT:     caught procedure missing dll 1
// PROC-NEXT:     caught procedure missing dll 1
// FHOOK-NEXT:    hook 4 missing
// FHOOK-NEXT:    missing(2) = -2
// LOADER-NEXT:   hook 4 missing
// FHOOK-NEXT:    missing(2) = -2

// MODULE:      caught module twice dll 1
// MODULE-NEXT: caught module twice dll 1
// MODULE-NEXT: caught module missing dll 1
// MODULE-NEXT: caught module missing dll 1

// LOAD-ALL:           load all caught missing
// LOAD-ALL-NEXT:      twice(2) = 4
// LOAD-ALL-HOOK:      hook 4 missing
// LOAD-ALL-HOOK-NEXT: load all 0
// LOAD-ALL-HOOK-NEXT: twice(2) = 4

// LOAD-ALL-OWN:      load all 0
// LOAD-ALL-OWN-NEXT: twice(2) = 4
// LOAD-ALL-OWN-NEXT: own 1 1 1

// ADDRESS:      before: loaded 0 0
// GONE-NEXT:    missing: loaded 0, in image 1 1 1 1 1 1, gate 1, same 1 1 1, kept 1
// OWN-NEXT:     after: own 1 1 1 1 1, local 1, stored 1
// KEPT-NEXT:    after: own 0 0 0 0 0, local 1, stored 1
// OWN-NEXT:     guards: own 1 1 1, offset 1
// KEPT-NEXT:    guards: own 0 0 0, offset 0
// OWN-NEXT:     two: own 1 1 1 1
// KEPT-NEXT:    two: own 0 0 0 0
// OWN-NEXT:     same 1 1 1 1 1
// OWN-NEXT:     read-only 1 1, calls 8 5 12
// GATE-NEXT:    gate 0 0
// OWN-NEXT:     stored after 2
// OWN-NEXT:     replayed: same 1, swapped 1 1

// KEEPS: keeps 1 1, two 1 1
// KEEPS-READ: keeps-read 1 1

// UNLOAD:         unloaded 1, still loaded 0
// UNLOAD-NEXT:    twice(4) = 8, loaded 1
// NO-UNLOAD:      unloaded 0, still loaded 1
// NO-UNLOAD-NOT:  twice

#include <delayimp.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef DLL
#  ifdef SECOND
__declspec(dllexport) int thrice(int Value) { return 3 * Value; }
#  else
__declspec(dllexport) int twice(int Value) { return 2 * Value; }
__declspec(dllexport) int add(int A, int B) { return A + B; }
// The name of the address 8 bytes into twice.
__asm__(".globl \"twice$ap8\"\n.set \"twice$ap8\", twice+8\n"
        ".section .drectve,\"yn\"\n.ascii \" -export:twice$ap8\"\n.text\n");
#    ifdef MISSING
__declspec(dllexport) int missing(int Value) { return Value; }
#    endif
#  endif
#else

__declspec(dllimport) int twice(int);
__declspec(dllimport) int add(int, int);
__declspec(dllimport) int missing(int);
extern IMAGE_DOS_HEADER __ImageBase;

#  ifdef WHOLE_SECTION
const BOOL __bChangeProtectionOfWholeDloadSection = TRUE;
#  endif

#  ifdef NOTIFY_HOOK
static FARPROC WINAPI notifyHook(unsigned Reason, DelayLoadInfo *Info) {
  static const char *const Stages[] = {"start", "pre-load", "pre-getproc",
                                       "fail-load", "fail-getproc", "end"};
  fprintf(stderr, "hook %s %s\n", Stages[Reason], Info->dlp.szProcName);
  return NULL;
}
const PfnDliHook __pfnDliNotifyHook2 = notifyHook;
#  endif

#  ifdef FAILURE_HOOK
static int substitute(int Value) { return -Value; }
static FARPROC WINAPI failureHook(unsigned Reason, DelayLoadInfo *Info) {
  fprintf(stderr, "hook %u %s\n", Reason, Info->dlp.szProcName);
  return Reason == dliFailGetProc ? (FARPROC)substitute : NULL;
}
const PfnDliHook __pfnDliFailureHook2 = failureHook;
#  endif

#  ifdef ADDRESS
#    ifdef GONE
#      define DLL_NAME "delayload.c.tmp-gone.dll"
#    else
#      define DLL_NAME "delayload.c.tmp.dll"
#    endif
#    define SECOND_NAME "delayload.c.tmp-2.dll"
__declspec(dllimport) int thrice(int);
// What the linker gives a DLL some of whose functions' addresses are its own.
extern void *volatile Gate __asm__("__gate_" DLL_NAME);
extern void *volatile Gate2 __asm__("__gate_" SECOND_NAME);
static int local(int Value) { return Value + 1; }
// The two DLLs' functions in one table.
int (*const volatile Two[])(int) = {twice, thrice};

// twice's address, taken at each placement of the guard: inline where the
// flags are dead in code that moves, which a fused read makes it; a lea; a
// load before code that reads the flags. viaOffset takes the address 8
// bytes into it. keeps takes it, and keepsTwo the second word of Two, with
// every other register, the flags and ymm0 to ymm4 set, and each returns
// whether all kept.
void *viaInline(void), *viaLea(void), *viaLoad(void), *viaOffset(void);
int keeps(void), keepsTwo(void), keepsRead(void);
// Whether Two[1] is the address given, by a compare in memory; and whether
// Swapped held the first address given, which it then holds the second of,
// by an exchange behind a lock of its own.
int sameTwo(int (*)(int)), swap(int (*)(int), int (*)(int));
int (*volatile Swapped)(int) = twice;
#    ifdef __x86_64__
__asm__(".section .text$guards,\"xr\"\n"
        ".globl viaInline, viaLea, viaLoad, viaOffset, keeps\n"
        "  movq __imp_add@FUSED(%rip), %rax\n"
        "viaInline:\n"
        "  movq __imp_twice(%rip), %rax\n"
        "  testq %rax, %rax\n"
        "  retq\n"
        "viaLea:\n"
        "  leaq twice(%rip), %rax\n"
        "  retq\n"
        "viaLoad:\n"
        "  movq __imp_twice(%rip), %rax\n"
        "  retq\n"
        "viaOffset:\n"
        "  leaq twice+8(%rip), %rax\n"
        "  retq\n"
        ".macro keeping load:vararg\n"
        "  pushq %rbx; pushq %rbp; pushq %rsi; pushq %rdi\n"
        "  pushq %r12; pushq %r13; pushq %r14; pushq %r15\n"
        "  .set .Lv, 0x5a5a0000\n"
        "  .irp r,rbx,rcx,rdx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15\n"
        "  .set .Lv, .Lv+1\n"
        "  movq $.Lv, %\\r\n"
        "  .endr\n"
        "  .irp n,0,1,2,3,4\n"
        "  vmovq %r1\\n, %xmm\\n\n"
        "  vpbroadcastq %xmm\\n, %ymm\\n\n"
        "  .endr\n"
        "  pushq $0x8d5\n"
        "  popfq\n"
        "  \\load\n"
        "  pushfq\n"
        "  popq %rax\n"
        "  andl $0xcd5, %eax\n"
        "  cmpl $0x8d5, %eax\n"
        "  jne 1f\n"
        "  .set .Lv, 0x5a5a0000\n"
        "  .irp r,rbx,rcx,rdx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15\n"
        "  .set .Lv, .Lv+1\n"
        "  cmpq $.Lv, %\\r\n"
        "  jne 1f\n"
        "  .endr\n"
        "  .irp n,0,1,2,3,4\n"
        "  vmovq %r1\\n, %xmm5\n"
        "  vpbroadcastq %xmm5, %ymm5\n"
        "  vpcmpeqq %ymm\\n, %ymm5, %ymm5\n"
        "  vpmovmskb %ymm5, %eax\n"
        "  cmpl $-1, %eax\n"
        "  jne 1f\n"
        "  .endr\n"
        "  movl $1, %eax\n"
        "  jmp 2f\n"
        "1:\n"
        "  xorl %eax, %eax\n"
        "2:\n"
        "  vzeroupper\n"
        "  popq %r15; popq %r14; popq %r13; popq %r12\n"
        "  popq %rdi; popq %rsi; popq %rbp; popq %rbx\n"
        "  retq\n"
        ".endm\n"
        ".globl keepsTwo, keepsRead, sameTwo, swap\n"
        "keeps:\n"
        "  keeping movq __imp_twice(%rip), %rax\n"
        "keepsTwo:\n"
        "  keeping movq Two+8(%rip), %rax\n"
        "keepsRead:\n"
        "  keeping movl Two+8(%rip), %eax\n"
        "sameTwo:\n"
        "  cmpq %rcx, Two+8(%rip)\n"
        "  sete %al\n"
        "  movzbl %al, %eax\n"
        "  retq\n"
        "swap:\n"
        "  movq %rcx, %rax\n"
        "  lock\n"
        "  cmpxchgq %rdx, Swapped(%rip)\n"
        "  sete %al\n"
        "  movzbl %al, %eax\n"
        "  retq\n"
        "  .text\n");
#    else
void *viaInline(void) { return (void *)twice; }
void *viaLea(void) { return (void *)twice; }
void *viaLoad(void) { return (void *)twice; }
void *viaOffset(void) { return (char *)twice + 8; }
int keeps(void) { return twice != NULL; }
int keepsTwo(void) { return Two[1] != NULL; }
int keepsRead(void) { return Two[1] != NULL; }
int sameTwo(int (*F)(int)) { return Two[1] == F; }
int swap(int (*Old)(int), int (*New)(int)) {
  return __sync_bool_compare_and_swap(&Swapped, Old, New);
}
#    endif

// A run that a word other than zero follows, a run that ends its section, a
// writable word, and one that the program stores to first.
int (*const volatile Table[])(int) = {twice, local};
int (*const volatile Last)(int) = twice;
int (*volatile Writable)(int) = twice;
int (*volatile Stored)(int) = twice;

static int inImage(const void *Pointer) {
  const char *Base = (const char *)&__ImageBase;
  const IMAGE_NT_HEADERS64 *Nt = (const IMAGE_NT_HEADERS64 *)(Base + __ImageBase.e_lfanew);
  return (size_t)((const char *)Pointer - Base) < Nt->OptionalHeader.SizeOfImage;
}

#  endif

// Whether the page of the first descriptor's address table is read-only.
static int tableIsReadOnly(void) {
  const char *Base = (const char *)&__ImageBase;
  const IMAGE_NT_HEADERS64 *Nt = (const IMAGE_NT_HEADERS64 *)(Base + __ImageBase.e_lfanew);
  const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor =
      (const IMAGE_DELAYLOAD_DESCRIPTOR *)(Base + Nt->OptionalHeader.DataDirectory
                                                      [IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                                                          .VirtualAddress);
  MEMORY_BASIC_INFORMATION Info;
  return VirtualQuery(Base + Descriptor->ImportAddressTableRVA, &Info, sizeof(Info)) &&
         Info.Protect == PAGE_READONLY;
}

#  define VCPP_EXCEPTION(Error) VcppException(ERROR_SEVERITY_ERROR, Error)
static DelayLoadInfo Captured;

static int filter(DWORD Code, EXCEPTION_POINTERS *Pointers) {
  if (Code != VCPP_EXCEPTION(ERROR_MOD_NOT_FOUND) && Code != VCPP_EXCEPTION(ERROR_PROC_NOT_FOUND))
    return EXCEPTION_CONTINUE_SEARCH;
  Captured = *(DelayLoadInfo *)Pointers->ExceptionRecord->ExceptionInformation[0];
  return EXCEPTION_EXECUTE_HANDLER;
}

static void call(const char *Name, int (*Function)(int)) {
  __try {
    fprintf(stderr, "%s(2) = %d\n", Name, Function(2));
  } __except (filter(GetExceptionCode(), GetExceptionInformation())) {
    fprintf(stderr, "caught %s %s %s %d\n",
            (DWORD)GetExceptionCode() == VCPP_EXCEPTION(ERROR_MOD_NOT_FOUND) ? "module"
                                                                           : "procedure",
            Captured.dlp.szProcName, Captured.szDll ? "dll" : "none",
            Captured.cb == sizeof(DelayLoadInfo));
  }
}

// Each call reads the import's address anew, since a function reads an
// address-table word once.
__attribute__((noinline)) static void callMissing(void) {
  call("missing", missing);
}

int main(int argc, char **argv) {
  const char *Mode = argv[1], *Dll = argv[2];
  if (!strcmp(Mode, "calls")) {
    if (GetModuleHandleA(Dll))
      return 1;
    fprintf(stderr, "read-only before %d\n", tableIsReadOnly());
    fprintf(stderr, "whole section %d\n", __bChangeProtectionOfWholeDloadSection);
    fprintf(stderr, "twice(21) = %d\n", twice(21));
    if (!GetModuleHandleA(Dll))
      return 2;
    fprintf(stderr, "add(1, 2) = %d\n", add(1, 2));
    fprintf(stderr, "load all %ld\n", (long)__HrLoadAllImportsForDll(Dll));
    fprintf(stderr, "load all of another %s\n",
            __HrLoadAllImportsForDll("no-such-library.dll") == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)
                ? "not found"
                : "found");
    fprintf(stderr, "read-only after %d\n", tableIsReadOnly());
  }
  if (!strcmp(Mode, "missing")) {
    call("twice", twice);
    call("twice", twice);
    callMissing();
    callMissing();
  }
  if (!strcmp(Mode, "load-all")) {
    __try {
      fprintf(stderr, "load all %ld\n", (long)__HrLoadAllImportsForDll(Dll));
    } __except (filter(GetExceptionCode(), GetExceptionInformation())) {
      fprintf(stderr, "load all caught %s\n", Captured.dlp.szProcName);
    }
    call("twice", twice);
#  ifdef ADDRESS
    FARPROC Own = GetProcAddress(GetModuleHandleA(Dll), "twice");
    fprintf(stderr, "own %d %d %d\n", (FARPROC)Table[0] == Own, (FARPROC)Last == Own,
            (FARPROC)Writable == Own);
#  endif
  }
#  ifdef ADDRESS
  if (!strcmp(Mode, "address")) {
    fprintf(stderr, "before: loaded %d %d\n", GetModuleHandleA(Dll) != NULL,
            GetModuleHandleA(SECOND_NAME) != NULL);
    // Read before anything else: the guards bind the DLLs.
    int (*Read)(int) = Table[0], (*ReadTwo)(int) = Two[1];
    int (*const volatile *Through)(int) = Two;
    int Swap = -1;
    if (argc > 3) {
      int (*volatile Code)(int) = twice;
      void *Lea = viaLea();
      int Kept = keeps();
      Swap = swap(Code, local);
      fprintf(stderr,
              "missing: loaded %d, in image %d %d %d %d %d %d, gate %d, same %d %d %d, kept %d\n",
              GetModuleHandleA(Dll) != NULL, inImage(Read), inImage(Table[0]), inImage(Last),
              inImage(Writable), inImage(Through[0]), inImage(Code), inImage(Gate), Read == Code,
              Lea == (void *)Code, Through[0] == Code, Kept);
      if (!CopyFileA(argv[3], argv[4], TRUE))
        return 5;
      Read = Table[0];
    }
    Stored = local;
    int (*volatile Code)(int) = twice;
    void *Inline = viaInline(), *Lea = viaLea(), *Load = viaLoad();
    MEMORY_BASIC_INFORMATION Info;
    FARPROC Own = GetProcAddress(GetModuleHandleA(Dll), "twice");
    FARPROC Own2 = GetProcAddress(GetModuleHandleA(SECOND_NAME), "thrice");
    fprintf(stderr, "after: own %d %d %d %d %d, local %d, stored %d\n", (FARPROC)Read == Own,
            (FARPROC)Table[0] == Own, (FARPROC)Last == Own, (FARPROC)Writable == Own,
            (FARPROC)Code == Own, Table[1] == local, Stored == local);
    fprintf(stderr, "guards: own %d %d %d, offset %d\n", (FARPROC)Inline == Own,
            (FARPROC)Lea == Own, (FARPROC)Load == Own, (char *)viaOffset() == (char *)Own + 8);
    fprintf(stderr, "two: own %d %d %d %d\n", (FARPROC)ReadTwo == Own2,
            (FARPROC)Through[0] == Own, (FARPROC)Through[1] == Own2, (FARPROC)thrice == Own2);
    fprintf(stderr, "same %d %d %d %d %d\n", Read == Code, Table[0] == Code,
            Inline == (void *)Code, ReadTwo == thrice, Through[1] == ReadTwo);
    fprintf(stderr, "read-only %d %d, calls %d %d %d\n",
            VirtualQuery((void *)&Table[0], &Info, sizeof(Info)) && Info.Protect == PAGE_READONLY,
            tableIsReadOnly(), Table[0](4), Table[1](4), Two[1](4));
    fprintf(stderr, "gate %d %d\n", inImage(Gate), inImage(Gate2));
    Writable = local;
    fprintf(stderr, "stored after %d\n", Writable(1));
    if (Swap < 0)
      Swap = swap(Code, local);
    fprintf(stderr, "replayed: same %d, swapped %d %d\n", sameTwo(thrice), Swap,
            Swapped == local);
  }
  if (!strcmp(Mode, "keeps-read")) {
    int Cold = keepsRead(), Hot = keepsRead();
    fprintf(stderr, "keeps-read %d %d\n", Cold, Hot);
  }
  if (!strcmp(Mode, "keeps")) {
    int Cold = keeps(), Hot = keeps(), ColdTwo = keepsTwo(), HotTwo = keepsTwo();
    fprintf(stderr, "keeps %d %d, two %d %d\n", Cold, Hot, ColdTwo, HotTwo);
  }
#  endif
  if (!strcmp(Mode, "unload")) {
    if (GetModuleHandleA(Dll) || twice(21) != 42 || !GetModuleHandleA(Dll) ||
        __FUnloadDelayLoadedDLL2("no-such-library.dll"))
      return 3;
    char Name[MAX_PATH];
    CharUpperA(strcpy(Name, Dll));
    BOOL Unloaded = __FUnloadDelayLoadedDLL2(Name);
    fprintf(stderr, "unloaded %d, still loaded %d\n", Unloaded, GetModuleHandleA(Dll) != NULL);
    if (Unloaded && __FUnloadDelayLoadedDLL2(Dll))
      return 4;
    // The table holds the thunks again, so the next call loads the DLL afresh.
    if (Unloaded)
      fprintf(stderr, "twice(4) = %d, loaded %d\n", twice(4), GetModuleHandleA(Dll) != NULL);
  }
  return 0;
}
#endif
