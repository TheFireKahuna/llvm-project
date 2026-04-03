// Test Control Flow Guard function table is present in load configuration.
//
// RUN: %clang_crt_main_cfg %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

// PE header structures (minimal).
typedef struct {
  uint16_t e_magic;
  uint16_t e_cblp;
  uint16_t e_cp;
  uint16_t e_crlc;
  uint16_t e_cparhdr;
  uint16_t e_minalloc;
  uint16_t e_maxalloc;
  uint16_t e_ss;
  uint16_t e_sp;
  uint16_t e_csum;
  uint16_t e_ip;
  uint16_t e_cs;
  uint16_t e_lfarlc;
  uint16_t e_ovno;
  uint16_t e_res[4];
  uint16_t e_oemid;
  uint16_t e_oeminfo;
  uint16_t e_res2[10];
  int32_t e_lfanew;
} IMAGE_DOS_HEADER;

#ifdef _WIN64
typedef struct {
  uint64_t Size;
  uint32_t TimeDateStamp;
  uint16_t MajorVersion;
  uint16_t MinorVersion;
  uint32_t GlobalFlagsClear;
  uint32_t GlobalFlagsSet;
  uint32_t CriticalSectionDefaultTimeout;
  uint64_t DeCommitFreeBlockThreshold;
  uint64_t DeCommitTotalFreeThreshold;
  uint64_t LockPrefixTable;
  uint64_t MaximumAllocationSize;
  uint64_t VirtualMemoryThreshold;
  uint64_t ProcessAffinityMask;
  uint32_t ProcessHeapFlags;
  uint16_t CSDVersion;
  uint16_t DependentLoadFlags;
  uint64_t EditList;
  uint64_t SecurityCookie;
  uint64_t SEHandlerTable;
  uint64_t SEHandlerCount;
  uint64_t GuardCFCheckFunctionPointer;
  uint64_t GuardCFDispatchFunctionPointer;
  uint64_t GuardCFFunctionTable;
  uint64_t GuardCFFunctionCount;
  uint32_t GuardFlags;
} IMAGE_LOAD_CONFIG_DIRECTORY;
#else
typedef struct {
  uint32_t Size;
  uint32_t TimeDateStamp;
  uint16_t MajorVersion;
  uint16_t MinorVersion;
  uint32_t GlobalFlagsClear;
  uint32_t GlobalFlagsSet;
  uint32_t CriticalSectionDefaultTimeout;
  uint32_t DeCommitFreeBlockThreshold;
  uint32_t DeCommitTotalFreeThreshold;
  uint32_t LockPrefixTable;
  uint32_t MaximumAllocationSize;
  uint32_t VirtualMemoryThreshold;
  uint32_t ProcessHeapFlags;
  uint32_t ProcessAffinityMask;
  uint16_t CSDVersion;
  uint16_t DependentLoadFlags;
  uint32_t EditList;
  uint32_t SecurityCookie;
  uint32_t SEHandlerTable;
  uint32_t SEHandlerCount;
  uint32_t GuardCFCheckFunctionPointer;
  uint32_t GuardCFDispatchFunctionPointer;
  uint32_t GuardCFFunctionTable;
  uint32_t GuardCFFunctionCount;
  uint32_t GuardFlags;
} IMAGE_LOAD_CONFIG_DIRECTORY;
#endif

// CFG flag values.
#define IMAGE_GUARD_CF_INSTRUMENTED 0x00000100
#define IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT 0x00000400

extern IMAGE_LOAD_CONFIG_DIRECTORY _load_config_used;

#define __stdcall __attribute__((ms_abi))

__declspec(dllimport) void *__stdcall GetModuleHandleW(const wchar_t *);

int main(void) {
  // CHECK: CFG function table test
  printf("CFG function table test\n");

  IMAGE_LOAD_CONFIG_DIRECTORY *lc = &_load_config_used;

  // CHECK: load config size >= min = 1
  printf("load config size >= min = %d\n",
         lc->Size >= sizeof(IMAGE_LOAD_CONFIG_DIRECTORY));

  // CHECK: security cookie set = 1
  printf("security cookie set = %d\n", lc->SecurityCookie != 0);

  // CHECK: guard flags present = 1
  uint32_t flags = lc->GuardFlags;
  int has_flags = (flags & IMAGE_GUARD_CF_INSTRUMENTED) != 0;
  printf("guard flags present = %d\n", has_flags);

  // CHECK: function table present = 1
  int has_table = (flags & IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT) != 0;
  printf("function table present = %d\n", has_table);

  // CHECK: guard check pointer set = 1
  printf("guard check pointer set = %d\n",
         lc->GuardCFCheckFunctionPointer != 0);

  // CHECK: guard dispatch pointer set = 1
  printf("guard dispatch pointer set = %d\n",
         lc->GuardCFDispatchFunctionPointer != 0);

  // CHECK: function table address set = 1
  printf("function table address set = %d\n", lc->GuardCFFunctionTable != 0);

  // CHECK: function count > 0 = 1
  printf("function count > 0 = %d\n", lc->GuardCFFunctionCount > 0);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
