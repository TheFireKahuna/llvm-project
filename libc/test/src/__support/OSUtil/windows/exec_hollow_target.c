/*===-- exec_hollow_target.c — Target for self-hollow research spike -------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===----------------------------------------------------------------------===
 *
 * Simple Win32 console program that prints its PID and some process state.
 * Used as the target for exec_hollow_research.exe to hollow into.
 *
 * If the self-hollow works correctly:
 *   - This program's output should show the SAME PID as the research program
 *   - The process identity (from the OS perspective) should still be the
 *     research program, but the code running is this program
 *
 * Build (standalone, no CRT — for self-hollow target):
 *   "C:/Program Files/LLVM/bin/clang.exe" -O2 -Wall -nostdlib              \
 *       -Wl,-entry:_start                                                   \
 *       libc/test/src/__support/OSUtil/windows/exec_hollow_target.c         \
 *       -lkernel32 -lntdll                                                  \
 *       -o exec_hollow_target.exe
 *
 * Build (with CRT — for normal use):
 *   "C:/Program Files/LLVM/bin/clang.exe" -O2 -Wall                        \
 *       libc/test/src/__support/OSUtil/windows/exec_hollow_target.c         \
 *       -lkernel32 -lntdll                                                  \
 *       -o exec_hollow_target.exe
 *
 *===----------------------------------------------------------------------===*/

#ifndef _WIN32
#error "This test is Windows-only"
#endif

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "ntdll.lib")

/* Minimal type defs */
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef const void *LPCVOID;
typedef unsigned short WCHAR;
typedef unsigned short USHORT;
typedef long NTSTATUS;
typedef unsigned long long ULONG_PTR;
typedef unsigned long long SIZE_T;

#define NULL ((void *)0)
#define STD_OUTPUT_HANDLE ((DWORD)-11)

typedef struct _UNICODE_STRING {
  USHORT Length;
  USHORT MaximumLength;
  WCHAR *Buffer;
} UNICODE_STRING;

typedef struct _RTL_USER_PROCESS_PARAMETERS {
  DWORD MaximumLength;
  DWORD Length;
  DWORD Flags;
  DWORD DebugFlags;
  HANDLE ConsoleHandle;
  DWORD ConsoleFlags;
  HANDLE StandardInput;
  HANDLE StandardOutput;
  HANDLE StandardError;
  UNICODE_STRING CurrentDirectoryPath;
  HANDLE CurrentDirectoryHandle;
  UNICODE_STRING DllPath;
  UNICODE_STRING ImagePathName;
  UNICODE_STRING CommandLine;
} RTL_USER_PROCESS_PARAMETERS;

typedef struct _LIST_ENTRY {
  struct _LIST_ENTRY *Flink;
  struct _LIST_ENTRY *Blink;
} LIST_ENTRY;

typedef struct _PEB_LDR_DATA {
  DWORD Length;
  unsigned char Initialized;
  HANDLE SsHandle;
  LIST_ENTRY InLoadOrderModuleList;
  LIST_ENTRY InMemoryOrderModuleList;
  LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA;

typedef struct _LDR_DATA_TABLE_ENTRY {
  LIST_ENTRY InLoadOrderLinks;
  LIST_ENTRY InMemoryOrderLinks;
  LIST_ENTRY InInitializationOrderLinks;
  void *DllBase;
  void *EntryPoint;
  DWORD SizeOfImage;
  UNICODE_STRING FullDllName;
  UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY;

typedef struct _PEB {
  unsigned char InheritedAddressSpace;
  unsigned char ReadImageFileExecOptions;
  unsigned char BeingDebugged;
  unsigned char BitField;
  unsigned char Padding0[4];
  void *Mutant;
  void *ImageBaseAddress;
  PEB_LDR_DATA *Ldr;
  RTL_USER_PROCESS_PARAMETERS *ProcessParameters;
} PEB;

__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) int __stdcall WriteFile(
    HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
    DWORD *lpNumberOfBytesWritten, void *lpOverlapped);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int uExitCode);
__declspec(dllimport) NTSTATUS __stdcall NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

/* Query MemoryMappedFilenameInformation for the image base */
__declspec(dllimport) NTSTATUS __stdcall NtQueryVirtualMemory(
    HANDLE ProcessHandle, void *BaseAddress, DWORD MemoryInformationClass,
    void *MemoryInformation, SIZE_T MemoryInformationLength,
    SIZE_T *ReturnLength);

#define NtCurrentProcess() ((HANDLE)(long long)-1)

static HANDLE g_out;

static void print(const char *s) {
  DWORD w;
  int len = 0;
  while (s[len]) len++;
  WriteFile(g_out, s, (DWORD)len, &w, NULL);
}

static void print_dec(unsigned long long v) {
  char buf[21];
  int pos = 20;
  buf[pos] = '\0';
  if (v == 0) { buf[--pos] = '0'; }
  else { while (v > 0) { buf[--pos] = '0' + (int)(v % 10); v /= 10; } }
  print(&buf[pos]);
}

static void print_hex(unsigned long long v) {
  char buf[19];
  buf[0] = '0'; buf[1] = 'x';
  for (int i = 15; i >= 0; i--) {
    int nib = (int)(v & 0xf);
    buf[2 + i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    v >>= 4;
  }
  buf[18] = '\0';
  print(buf);
}

static void print_wstr(const WCHAR *s, int len) {
  char buf[512];
  int max = (len < 511) ? len : 511;
  for (int i = 0; i < max && s[i]; i++)
    buf[i] = (s[i] < 128) ? (char)s[i] : '?';
  buf[(max < 511) ? max : 511] = '\0';
  print(buf);
}

int main(int argc, char **argv) {
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);

  print("\n");
  print("================================================================\n");
  print("  exec_hollow_target — I AM THE TARGET PROGRAM\n");
  print("================================================================\n");
  print("\n");

  DWORD pid = GetCurrentProcessId();
  print("  PID:              "); print_dec(pid); print("\n");

  /* Read PEB */
  PEB *peb;
  __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));

  print("  ImageBaseAddress: "); print_hex((ULONG_PTR)peb->ImageBaseAddress); print("\n");
  print("  InheritedAddrSpc: "); print_dec(peb->InheritedAddressSpace); print("\n");
  print("  Ldr->Initialized:"); print_dec(peb->Ldr->Initialized); print("\n");
  print("  ImagePathName:    ");
  print_wstr(peb->ProcessParameters->ImagePathName.Buffer,
             peb->ProcessParameters->ImagePathName.Length / sizeof(WCHAR));
  print("\n");
  print("  CommandLine:      ");
  print_wstr(peb->ProcessParameters->CommandLine.Buffer,
             peb->ProcessParameters->CommandLine.Length / sizeof(WCHAR));
  print("\n");

  /* Query the filename of the mapped image at our base address */
  print("\n  MemoryMappedFilenameInformation at image base:\n");
  struct {
    UNICODE_STRING Name;
    WCHAR Buffer[512];
  } filename_info;

  SIZE_T result_len = 0;
  NTSTATUS status = NtQueryVirtualMemory(
      NtCurrentProcess(),
      peb->ImageBaseAddress,
      2, /* MemoryMappedFilenameInformation */
      &filename_info,
      sizeof(filename_info),
      &result_len);

  if (status >= 0) {
    print("    ");
    print_wstr(filename_info.Name.Buffer, filename_info.Name.Length / sizeof(WCHAR));
    print("\n");
  } else {
    print("    (query failed: "); print_hex((unsigned long long)(unsigned int)status); print(")\n");
  }

  /* List loaded modules */
  print("\n  Loaded modules:\n");
  LIST_ENTRY *head = &peb->Ldr->InLoadOrderModuleList;
  LIST_ENTRY *entry = head->Flink;
  int count = 0;
  while (entry != head && count < 32) {
    LDR_DATA_TABLE_ENTRY *mod = (LDR_DATA_TABLE_ENTRY *)entry;
    print("    ["); print_dec(count); print("] ");
    print_wstr(mod->BaseDllName.Buffer, mod->BaseDllName.Length / sizeof(WCHAR));
    print("  base="); print_hex((ULONG_PTR)mod->DllBase);
    print("\n");
    entry = entry->Flink;
    count++;
  }

  print("\n");
  print("================================================================\n");
  print("  If the PID above matches the research program's PID,\n");
  print("  the self-hollow worked! This code is running in the\n");
  print("  original process with the original PID.\n");
  print("================================================================\n");
  print("\n");

  /* Check argc/argv */
  print("  argc: "); print_dec(argc); print("\n");
  for (int i = 0; i < argc && i < 10; i++) {
    print("  argv["); print_dec(i); print("]: ");
    if (argv[i]) print(argv[i]);
    else print("(null)");
    print("\n");
  }
  print("\n");

  return 42; /* Distinctive exit code to verify we ran */
}

/* Standalone entry point for -nostdlib builds.
 * Calls main() directly and exits via NtTerminateProcess (avoids
 * ExitProcess which walks DLL detach and may crash with stale state). */
void _start(void) {
  int ret = main(0, (char **)NULL);
  NtTerminateProcess((HANDLE)(long long)-1, (NTSTATUS)ret);
  __builtin_unreachable();
}
