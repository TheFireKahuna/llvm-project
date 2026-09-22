// Independent capability expectation for integration tests. Do not infer support
// from aligned_alloc itself: an implementation returning null must fail here.
#ifndef WINCRT_TEST_ALIGNED_ALLOC_H
#define WINCRT_TEST_ALIGNED_ALLOC_H
#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <string.h>
#include <windows.h>

static size_t test_max_alignment(void) {
#if defined(__x86_64__)
  const unsigned char *image =
      (const unsigned char *)GetModuleHandleW(L"ntdll.dll");
  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
  const IMAGE_NT_HEADERS64 *nt =
      (const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY *dir =
      &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  const IMAGE_DEBUG_DIRECTORY *debug =
      (const IMAGE_DEBUG_DIRECTORY *)(image + dir->VirtualAddress);
  const unsigned char identity[] = {
      0x52, 0x53, 0x44, 0x53, 0x21, 0xbf, 0x5b, 0x0d, 0x19, 0x0a, 0x91, 0xb6,
      0x55, 0x91, 0xa9, 0xbb, 0x85, 0x2f, 0x41, 0x7a, 0x01, 0x00, 0x00, 0x00};
  uint32_t signature;
  memcpy(&signature, (const unsigned char *)GetProcessHeap() + 0x10,
         sizeof(signature));
  if (signature != 0xddeeddee)
    return 16;
  for (size_t i = 0; i < dir->Size / sizeof(*debug); ++i)
    if (debug[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW &&
        debug[i].SizeOfData >= sizeof(identity) &&
        memcmp(image + debug[i].AddressOfRawData, identity, sizeof(identity)) ==
            0)
      return (size_t)1 << 26;
#endif
  return 16;
}
#endif
