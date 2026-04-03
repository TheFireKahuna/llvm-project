// Test _load_config_used PE Load Configuration Directory structure.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN64
extern uint64_t __security_cookie;
#else
extern uint32_t __security_cookie;
#endif

// Minimal _load_config_used layout for fields under test.
#ifdef _WIN64
struct LoadConfigDir {
  uint32_t Size;
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
  uint64_t SecurityCookie;          // Offset 0x58
  uint64_t SEHandlerTable;
  uint64_t SEHandlerCount;
  uint64_t GuardCFCheckFunctionPointer;  // Offset 0x70
  uint64_t GuardCFDispatchFunctionPointer;
  uint64_t GuardCFFunctionTable;
  uint64_t GuardCFFunctionCount;
  uint32_t GuardFlags;              // Offset 0x90
};
#else
struct LoadConfigDir {
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
  uint32_t SecurityCookie;          // Offset 0x3C
  uint32_t SEHandlerTable;
  uint32_t SEHandlerCount;
  uint32_t GuardCFCheckFunctionPointer;
  uint32_t GuardCFDispatchFunctionPointer;
  uint32_t GuardCFFunctionTable;
  uint32_t GuardCFFunctionCount;
  uint32_t GuardFlags;
};
#endif

extern const struct LoadConfigDir _load_config_used;

int main() {
  printf("Load config test\n");

  printf("_load_config_used address = %p\n", (void*)&_load_config_used);
  printf("_load_config_used.Size = %u\n", _load_config_used.Size);

#ifdef _WIN64
  int size_valid = (_load_config_used.Size >= 0x94);  // Minimum for CFG on x64
#else
  int size_valid = (_load_config_used.Size >= 0x5C);  // Minimum for CFG on x86
#endif
  // CHECK: size valid = 1
  printf("size valid = %d\n", size_valid);

  int cookie_ptr_valid =
      (_load_config_used.SecurityCookie == (uintptr_t)&__security_cookie);
  // CHECK: cookie pointer valid = 1
  printf("cookie pointer valid = %d\n", cookie_ptr_valid);

  int guard_flags_valid = ((_load_config_used.GuardFlags & 0x500) == 0x500);
  // CHECK: guard flags valid = 1
  printf("guard flags valid = %d\n", guard_flags_valid);

  int check_ptr_set = (_load_config_used.GuardCFCheckFunctionPointer != 0);
  // CHECK: check pointer set = 1
  printf("check pointer set = %d\n", check_ptr_set);

  printf("GuardCFCheckFunctionPointer = 0x%llx\n",
         (unsigned long long)_load_config_used.GuardCFCheckFunctionPointer);
  printf("GuardCFDispatchFunctionPointer = 0x%llx\n",
         (unsigned long long)_load_config_used.GuardCFDispatchFunctionPointer);
  printf("GuardFlags = 0x%x\n", _load_config_used.GuardFlags);

  // CHECK: PASS
  if (size_valid && cookie_ptr_valid && guard_flags_valid && check_ptr_set) {
    printf("PASS\n");
    return 0;
  } else {
    printf("FAIL\n");
    return 1;
  }
}
