// Deterministic resource-failure tests of the production large-allocation code.
// Native calls are replaced by explicit template specializations in this test
// translation unit; the runtime has no fault-injection branches or switches.
// RUN: %clang_crt_main -std=c++17 -O2 -mguard=cf -fms-extensions -fno-exceptions -fno-rtti -UNDEBUG -Wall -Wextra -Werror %s -o %t.exe -Wl,/guard:cf
// RUN: %run %t.exe
// REQUIRES: windows, crt, x86_64-target-arch
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static unsigned Registrations;
static BOOL WINAPI rejectRegistration(HANDLE, PVOID, SIZE_T, ULONG,
                                      PCFG_CALL_TARGET_INFO) {
  ++Registrations;
  return FALSE;
}
#define SetProcessValidCallTargets rejectRegistration

namespace {
using MetaFn = void *(*)(size_t, size_t, unsigned, void *);
using VAFn = long (*)(void **, size_t *, size_t, unsigned, unsigned, void *);
using QueryFn = size_t (*)(void *, void *, void ***, unsigned *);
using LimitFn = bool (*)(size_t, void *);
using MetaFreeFn = void (*)(void *, void *);
using VAFreeFn = void (*)(void *, void **, size_t *);
using InsertFn = void (*)(void *, void *, unsigned char, void *);
using LogFn = void (*)(void *, void *, size_t);
template <class T> T entry(uintptr_t, size_t) noexcept;
template <> MetaFn entry<MetaFn>(uintptr_t, size_t) noexcept;
template <> VAFn entry<VAFn>(uintptr_t, size_t) noexcept;
template <> QueryFn entry<QueryFn>(uintptr_t, size_t) noexcept;
template <> LimitFn entry<LimitFn>(uintptr_t, size_t) noexcept;
template <> MetaFreeFn entry<MetaFreeFn>(uintptr_t, size_t) noexcept;
template <> VAFreeFn entry<VAFreeFn>(uintptr_t, size_t) noexcept;
template <> InsertFn entry<InsertFn>(uintptr_t, size_t) noexcept;
template <> LogFn entry<LogFn>(uintptr_t, size_t) noexcept;
} // namespace
#include "../../../../lib/wincrt/aligned_alloc.cpp"

namespace {
alignas(16) Byte FakeHeap[0xa00];
alignas(16) Byte FakeNode[40];
unsigned Failure, MetaLive, VALive, Insertions;
size_t Reserved;
void *Owner;

void *metadata(size_t size, size_t requested, unsigned kind, void *) {
  assert(size == 40 && requested == 40 && kind == 0);
  if (Failure == 1)
    return nullptr;
  ++MetaLive;
  return FakeNode;
}
long va(void **base, size_t *size, size_t alignment, unsigned type,
        unsigned protection, void *) {
  assert(protection == PAGE_READWRITE);
  if (type == MEM_RESERVE) {
    if (Failure == 2)
      return -1;
    if (Failure == 6) {
      // Failed native calls may alter output slots without acquiring a resource.
      // Rollback must track successful acquisition, not infer it from *base.
      *base = reinterpret_cast<void *>(uintptr_t{1} << 40);
      *size = VAUnit;
      return -1;
    }
    assert(alignment && *size >= alignment);
    *base = reinterpret_cast<void *>(uintptr_t(1) << 40);
    *size = (*size + VAUnit - 1) & -VAUnit;
    Reserved = *size;
    ++VALive;
    return 0;
  }
  assert(type == MEM_COMMIT && alignment == 0 && *size < Reserved);
  return Failure == 5 ? -1 : 0;
}
size_t query(void *, void *, void ***owner, unsigned *) {
  if (Failure == 3)
    return 0;
  if (Failure == 7)
    return Reserved; // No owner slot: release the acquired reservation and node.
  *owner = &Owner;
  return Reserved;
}
bool limit(size_t, void *heap) {
  assert(heap == FakeHeap && Owner == heap);
  return Failure != 4;
}
void freeMetadata(void *heap, void *node) {
  assert(heap == FakeHeap && node == FakeNode && MetaLive == 1);
  assert(VALive == 0); // Release the reservation before its metadata.
  --MetaLive;
}
void freeVA(void *, void **base, size_t *size) {
  assert(*base == reinterpret_cast<void *>(uintptr_t(1) << 40));
  assert(*size == Reserved && VALive == 1);
  --VALive;
  Owner = nullptr;
  *base = nullptr;
  *size = 0;
}
void insert(void *tree, void *parent, unsigned char right, void *node) {
  assert(tree == FakeHeap + 0x48 && !parent && !right && node == FakeNode);
  assert(MetaLive == 1 && VALive == 1);
  ++Insertions;
}
void log(void *, void *, size_t) {}

template <> MetaFn entry<MetaFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == MetadataAlloc);
  return metadata;
}
template <> VAFn entry<VAFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == AllocVA);
  return va;
}
template <> QueryFn entry<QueryFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == QueryVA);
  return query;
}
template <> LimitFn entry<LimitFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == CheckCommitLimit);
  return limit;
}
template <> MetaFreeFn entry<MetaFreeFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == MetadataFree);
  return freeMetadata;
}
template <> VAFreeFn entry<VAFreeFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == FreeVA);
  return freeVA;
}
template <> InsertFn entry<InsertFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == InsertNode);
  return insert;
}
template <> LogFn entry<LogFn>(uintptr_t, size_t rva) noexcept {
  assert(rva == LogRangeReserve);
  return log;
}
} // namespace

int main() {
  for (unsigned failure = 1; failure <= 7; ++failure) {
    Failure = failure;
    assert(allocateLarge(0, FakeHeap, 1 << 27, 1048577) == nullptr);
    assert(MetaLive == 0 && VALive == 0 && Insertions == 0);
    assert(read<size_t>(FakeHeap + 0x58) == 0);
    assert(read<size_t>(FakeHeap + 0x60) == 0);
  }
  Failure = 0;
  assert(allocateLarge(0, FakeHeap, size_t(1) << 63, 17) == nullptr);
  assert(allocateLarge(0, FakeHeap, 4096, PTRDIFF_MAX) == nullptr);
  assert(MetaLive == 0 && VALive == 0);
  void *p = allocateLarge(0, FakeHeap, size_t(1) << 34, 1048577);
  assert(p && MetaLive == 1 && VALive == 1 && Insertions == 1);
  // Exact requested size survives native large-node size decoding. Padding
  // remains reserved; it must not appear as committed payload.
  const size_t packed = read<size_t>(FakeNode + 32);
  const size_t committed = packed & -PageSize;
  assert(committed - (read<size_t>(FakeNode + 24) & 65535) == 1048577);
  assert(read<size_t>(FakeHeap + 0x58) == Reserved / PageSize);
  assert(read<size_t>(FakeHeap + 0x60) == committed / PageSize);
  assert(committed < Reserved && (packed & 2));

  alignas(16) Byte image[4096]{};
  assert(!matchesImage(image));
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(image);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = -1;
  assert(!matchesImage(image));
  dos->e_lfanew = 4096;
  assert(!matchesImage(image));
  dos->e_lfanew = 128;
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(image + 128);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfImage = 0x247000;
  nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
  auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  dir.VirtualAddress = 512;
  dir.Size = sizeof(IMAGE_DEBUG_DIRECTORY);
  auto *debug = reinterpret_cast<IMAGE_DEBUG_DIRECTORY *>(image + 512);
  debug->Type = IMAGE_DEBUG_TYPE_CODEVIEW;
  debug->AddressOfRawData = 1024;
  debug->SizeOfData = 24;
  // Valid PE headers do not establish the private ABI identity.
  assert(!matchesImage(image));
  constexpr Byte identity[] = {0x52, 0x53, 0x44, 0x53, 0x21, 0xbf, 0x5b, 0x0d,
                               0x19, 0x0a, 0x91, 0xb6, 0x55, 0x91, 0xa9, 0xbb,
                               0x85, 0x2f, 0x41, 0x7a, 0x01, 0x00, 0x00, 0x00};
  memcpy(image + 1024, identity, sizeof(identity));
  assert(matchesImage(image));
  ++image[1024 + 4]; // A different GUID must not qualify.
  assert(!matchesImage(image));
  --image[1024 + 4];
  ++image[1024 + 20]; // The PDB age is part of the contract too.
  assert(!matchesImage(image));
  --image[1024 + 20];
  debug->AddressOfRawData = 0x246fff;
  // Reject the range before reading outside the image.
  assert(!matchesImage(image));
  dir.Size = UINT32_MAX;
  assert(!matchesImage(image));
  // Exercise publication of a denied CFG capability through the public entry,
  // without mutating OS policy or adding a hook to the production runtime.
  assert(NativeImage == 0);
  void *output = image;
  errno = EDOM;
  assert(posix_memalign(&output, 64, 17) == ENOMEM);
  assert(output == image && errno == EDOM);
  assert(NativeImage == 1);
  errno = 0;
  assert(aligned_alloc(64, 17) == nullptr && errno == EINVAL);
  assert(NativeImage == 1);
  if (matchesImage(
          reinterpret_cast<const Byte *>(GetModuleHandleW(L"ntdll.dll"))))
    assert(Registrations == 1);
  const unsigned attempts = Registrations;
  assert(aligned_alloc(4096, 17) == nullptr);
  assert(Registrations == attempts);
  void *baseline = aligned_alloc(16, 17);
  assert(baseline && (reinterpret_cast<uintptr_t>(baseline) & 15) == 0);
  free(baseline);
}
