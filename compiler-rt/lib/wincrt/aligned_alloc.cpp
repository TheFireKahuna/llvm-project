//===-- aligned_alloc.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Return native heap blocks that the shared release UCRT can free unchanged.
// Fundamental alignment uses the public heap API. Extended alignment requires
// the exact ntdll revision below and preserves its allocation ownership,
// metadata and heap-walk protocol. No allocation registry or free shim is used.

#include "heap.h"
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#if defined(__x86_64__)
// Use the SDK import: this sensitive API rejects CFG indirect calls, even
// through a pointer returned by GetProcAddress.
#pragma comment(lib, "onecore.lib")

// Exported by the SDK's ntdll.lib, but not declared by its public headers.
extern "C" __declspec(dllimport) LONG NTAPI
RtlWaitOnAddress(const void *, const void *, SIZE_T, const LARGE_INTEGER *);
#endif

namespace {
static_assert(__atomic_always_lock_free(sizeof(void *), nullptr));
static_assert(__atomic_always_lock_free(sizeof(unsigned), nullptr));

constexpr size_t NativeAlignment = 16;

// Keep invalid input separate from an unavailable native allocation path.
// POSIX reserves EINVAL for invalid alignment; valid requests that cannot be
// serviced report ENOMEM. aligned_alloc retains its existing EINVAL result for
// an unqualified heap or native ABI.
enum class AllocationError { InvalidAlignment, Unavailable, OutOfMemory };

constexpr bool validAlignment(size_t Alignment, size_t Minimum) {
  return Alignment >= Minimum && (Alignment & (Alignment - 1)) == 0;
}

constexpr int alignedAllocError(AllocationError Error) {
  return Error == AllocationError::OutOfMemory ? ENOMEM : EINVAL;
}

constexpr int posixMemalignError(AllocationError Error) {
  return Error == AllocationError::InvalidAlignment ? EINVAL : ENOMEM;
}

// The PEB is only an observation to verify. UCRT supplies the process-lifetime
// owner. This atomic publishes no dependent data, so relaxed ordering suffices.
void *AllocationHeap;

[[gnu::cold, gnu::noinline]] void *initializeAllocationHeap() {
  void *const Owner = reinterpret_cast<void *>(_get_heap_handle());
  if (!Owner)
    return nullptr;
  void *Expected = nullptr;
  return __atomic_compare_exchange_n(&AllocationHeap, &Expected, Owner, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)
             ? Owner
             : Expected;
}

#if defined(__x86_64__)
using Byte = unsigned char;
constexpr size_t PageSize = 4096;
constexpr size_t VAUnit = size_t{1} << 20;
constexpr size_t MinReservationAlignment = 2 * VAUnit;
constexpr size_t MaxSegmentAlignment = 65536;
constexpr size_t MaxCompactSize = PageSize / 2;
constexpr uintptr_t UnavailableImage = 1;
constexpr unsigned HeapLocked = 0x10;
constexpr uint64_t DefaultEnvironmentAttributes = 0x1000000;
constexpr size_t HeapWalkContext = 0x1858;
constexpr size_t HeapFeatures = 0x1ce708;
constexpr size_t VAManager = 0x1ce988;

// Offsets in the qualified segment heap, not a C++ overlay of native storage.
namespace HeapOffset {
constexpr size_t Flags = 0x14;
constexpr size_t LargeLock = 0x40;
constexpr size_t LargeTree = 0x48;
constexpr size_t ReservedPages = 0x58;
constexpr size_t CommittedPages = 0x60;
constexpr size_t LockOwner = 0xe0;
constexpr size_t SmallContext = 0x140;
constexpr size_t LargeContext = 0x200;
} // namespace HeapOffset

namespace ContextOffset {
constexpr size_t UnitShift = 8;
constexpr size_t MaximumSize = 0x10;
} // namespace ContextOffset

namespace LargeNode {
constexpr size_t Bytes = 40;
constexpr size_t Address = 24;
constexpr size_t Size = 32;
constexpr uintptr_t AddressMask = -uintptr_t{65536};
constexpr size_t GuardPage = 2;
constexpr unsigned AlignmentShift = 2;
} // namespace LargeNode

// Local RTL_HP_ENV_HANDLE snapshot, copied without aliasing native storage.
struct alignas(16) HeapEnvironment {
  uint64_t Attributes;
  uintptr_t Context;
};
static_assert(sizeof(HeapEnvironment) == 16);
static_assert(offsetof(HeapEnvironment, Context) == 8);

struct CodeViewIdentity {
  uint32_t Signature;
  GUID Guid;
  uint32_t Age;
};
static_assert(sizeof(CodeViewIdentity) == 24);
static_assert(offsetof(CodeViewIdentity, Guid) == 4);
static_assert(offsetof(CodeViewIdentity, Age) == 20);

// x64 ntdll 10.0.26100.9539. RSDS identifies a compatible private ABI; it does
// not authenticate the loaded image against arbitrary memory corruption.
constexpr CodeViewIdentity ExpectedIdentity{
    0x53445352, // RSDS
    {0x0d5bbf21,
     0x0a19,
     0xb691,
     {0x55, 0x91, 0xa9, 0xbb, 0x85, 0x2f, 0x41, 0x7a}},
    1};

// Complete, 16-byte-aligned entries, never rounded CFG buckets or mid-function
// branches. The metadata-free and VA-query wrappers retain native environment
// handling and avoid the unaligned inner entries.
constexpr size_t SegAlloc = 0x40560;
constexpr size_t MetadataAlloc = 0x30b90;
constexpr size_t AllocVA = 0x64750;
constexpr size_t QueryVA = 0x65ea0;
constexpr size_t CheckCommitLimit = 0x81540;
constexpr size_t FreeVA = 0x64cc0;
constexpr size_t MetadataFree = 0x7f930;
constexpr size_t InsertNode = 0x440a0;
constexpr size_t LogRangeReserve = 0x117560;

// Qualified native ABIs. Keep call-site casts out of the allocation logic.
using AllocateMetadata = void *(*)(size_t, size_t, unsigned, void *);
using AllocateVirtualMemory = long (*)(void **, size_t *, size_t, unsigned,
                                       unsigned, void *);
using QueryVirtualMemory = size_t (*)(void *, void *, void ***, unsigned *);
using CheckHeapCommitLimit = bool (*)(size_t, void *);
using FreeMetadata = void (*)(void *, void *);
using FreeVirtualMemory = void (*)(void *, void **, size_t *);
using InsertTreeNode = void (*)(void *, void *, unsigned char, void *);
using LogReservation = void (*)(void *, void *, size_t);
using AllocateSegment = void *(*)(void *, size_t, size_t, size_t, unsigned);

// This thread owns the marker; RTL's exclusive heap walker observes it.
// Preserve each aligned store, including withdrawal before waiting.
void setHeapWalkContext(uintptr_t Value) {
  using GSWord = uintptr_t __attribute__((address_space(256)));
  *reinterpret_cast<volatile GSWord *>(HeapWalkContext) = Value;
}

// Copy native storage into local values without imposing C++ object lifetime,
// alignment or aliasing requirements on that storage.
template <class T> T read(const void *Address) noexcept {
  static_assert(__is_trivially_copyable(T));
  T Value;
  __builtin_memcpy(&Value, Address, sizeof(Value));
  return Value;
}
template <class T> void write(void *Address, T Value) noexcept {
  static_assert(__is_trivially_copyable(T));
  __builtin_memcpy(Address, &Value, sizeof(Value));
}
template <class T> T entry(uintptr_t Image, size_t RVA) noexcept {
  return reinterpret_cast<T>(Image + RVA);
}

// 0: unresolved, 1: unavailable, otherwise the qualified base. No initializing
// state to wait on under loader lock: concurrent initializers make identical,
// idempotent registrations and publish only after completing qualification.
uintptr_t NativeImage;

constexpr bool contains(size_t ImageSize, size_t Offset, size_t Size) {
  return Offset <= ImageSize && Size <= ImageSize - Offset;
}

bool matchesImage(const Byte *Image) {
  const auto *DOS = reinterpret_cast<const IMAGE_DOS_HEADER *>(Image);
  if (DOS->e_magic != IMAGE_DOS_SIGNATURE || DOS->e_lfanew < 0 ||
      static_cast<size_t>(DOS->e_lfanew) >
          PageSize - sizeof(IMAGE_NT_HEADERS64))
    return false;
  const auto *NT =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(Image + DOS->e_lfanew);
  if (NT->Signature != IMAGE_NT_SIGNATURE ||
      NT->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      NT->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      NT->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
    return false;
  const size_t ImageSize = NT->OptionalHeader.SizeOfImage;
  const auto &Directory =
      NT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (!contains(ImageSize, VAManager, 0x900) ||
      !contains(ImageSize, Directory.VirtualAddress, Directory.Size) ||
      Directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY))
    return false;
  const auto *Debug = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY *>(
      Image + Directory.VirtualAddress);
  const size_t Count = Directory.Size / sizeof(*Debug);
  for (size_t I = 0; I != Count; ++I) {
    const auto &Record = Debug[I];
    if (Record.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
        Record.SizeOfData < sizeof(ExpectedIdentity) ||
        !contains(ImageSize, Record.AddressOfRawData, Record.SizeOfData))
      continue;
    return __builtin_memcmp(Image + Record.AddressOfRawData, &ExpectedIdentity,
                            sizeof(ExpectedIdentity)) == 0;
  }
  return false;
}

uintptr_t publishNativeImage(uintptr_t Result) {
  uintptr_t Expected = 0;
  // Publish completed qualification; a losing initializer acquires the winner.
  return __atomic_compare_exchange_n(&NativeImage, &Expected, Result, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
             ? Result
             : Expected;
}

[[gnu::cold, gnu::noinline]] uintptr_t initializeNativeImage() {
  auto *Image = reinterpret_cast<Byte *>(GetModuleHandleW(L"ntdll.dll"));
  if (!Image || !matchesImage(Image))
    return publishNativeImage(UnavailableImage);

  PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY Policy{};
  if (!GetProcessMitigationPolicy(GetCurrentProcess(),
                                  ProcessControlFlowGuardPolicy, &Policy,
                                  sizeof(Policy)))
    return publishNativeImage(UnavailableImage);

  if (Policy.EnableControlFlowGuard) {
    constexpr size_t Targets[] = {
        SegAlloc, MetadataAlloc, AllocVA,    QueryVA,        CheckCommitLimit,
        FreeVA,   MetadataFree,  InsertNode, LogRangeReserve};
    for (const size_t RVA : Targets) {
      CFG_CALL_TARGET_INFO Target{RVA & (PageSize - 1), CFG_CALL_TARGET_VALID};
      if (!SetProcessValidCallTargets(GetCurrentProcess(),
                                      Image + (RVA & -PageSize), PageSize, 1,
                                      &Target))
        return publishNativeImage(UnavailableImage);
    }
  }
  return publishNativeImage(reinterpret_cast<uintptr_t>(Image));
}

// Roll back ordinary failure returns until tree insertion transfers ownership
// to RTL. This scope owner does not recover from SEH corruption exceptions;
// those propagate through the enclosing marker-cleanup boundary.
class PendingLargeAllocation {
  const uintptr_t Image;
  Byte *const Heap;
  Byte *const Node;
  enum class State { MetadataOnly, Reserved, Published };
  State Ownership = State::MetadataOnly;

public:
  // Native in/out slots: keep the allocation and rollback calls on these same
  // variables. Output values alone do not establish ownership after failure.
  void *Base = nullptr;
  size_t Reserve;

  PendingLargeAllocation(uintptr_t Image, Byte *Heap, Byte *Node,
                         size_t Reserve)
      : Image(Image), Heap(Heap), Node(Node), Reserve(Reserve) {}
  PendingLargeAllocation(const PendingLargeAllocation &) = delete;
  PendingLargeAllocation &operator=(const PendingLargeAllocation &) = delete;

  ~PendingLargeAllocation() {
    if (Ownership == State::Published)
      return;
    if (Ownership == State::Reserved)
      entry<FreeVirtualMemory>(Image, FreeVA)(
          reinterpret_cast<void *>(Image + VAManager), &Base, &Reserve);
    entry<FreeMetadata>(Image, MetadataFree)(Heap, Node);
  }

  void didReserve() { Ownership = State::Reserved; }

  [[nodiscard]] void *release() {
    Ownership = State::Published;
    return Base;
  }
};

[[gnu::cold, gnu::noinline]] unsigned waitForHeapUnlock(Byte *Heap,
                                                        unsigned Flags) {
  auto *const FlagsAddress =
      reinterpret_cast<unsigned *>(Heap + HeapOffset::Flags);
  auto *const OwnerAddress =
      reinterpret_cast<unsigned *>(Heap + HeapOffset::LockOwner);
  const unsigned ThreadId = __readgsdword(0x48);

  // HeapLock's owner may allocate. Other threads withdraw their markers so
  // the exclusive walker can finish, then publish and recheck after every wake.
  // RtlWaitOnAddress uses the native waiter's address key and spin budget.
  while ((Flags & HeapLocked) != 0) {
    if (__atomic_load_n(OwnerAddress, __ATOMIC_RELAXED) == ThreadId)
      break;
    setHeapWalkContext(0);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    RtlWaitOnAddress(FlagsAddress, &Flags, sizeof(Flags), nullptr);
    setHeapWalkContext(reinterpret_cast<uintptr_t>(FlagsAddress));
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    Flags = __atomic_load_n(FlagsAddress, __ATOMIC_RELAXED);
  }
  return Flags & ~HeapLocked;
}

// The caller has entered the heap-walk protocol and checked the environment.
// A successful result is a native large allocation, including RTL metadata,
// owner slot, guard page, encoded tree membership and accounting.
void *allocateLarge(uintptr_t Image, Byte *Heap, size_t Alignment,
                    size_t Size) {
  // Check representability before acquiring either resource. Native exhaustion
  // remains an ordinary allocation failure.
  if (Alignment > PTRDIFF_MAX)
    return nullptr;
  const size_t Granularity =
      Alignment > MinReservationAlignment ? Alignment : MinReservationAlignment;
  if (Size > PTRDIFF_MAX - (Granularity - 1) - PageSize)
    return nullptr;

  // Keep a stable descriptor across metadata, reservation and commit calls.
  auto Environment = read<HeapEnvironment>(Heap);
  auto *const Node =
      static_cast<Byte *>(entry<AllocateMetadata>(Image, MetadataAlloc)(
          LargeNode::Bytes, LargeNode::Bytes, 0, &Environment));
  if (!Node)
    return nullptr;
  __builtin_memset(Node, 0, LargeNode::Bytes);

  // Reserve alignment padding and a native guard page, but commit only payload.
  const size_t RequiredReserve = Size + PageSize;
  PendingLargeAllocation Allocation(
      Image, Heap, Node,
      RequiredReserve > Alignment ? RequiredReserve : Alignment);
  const auto AllocateVA = entry<AllocateVirtualMemory>(Image, AllocVA);
  if (AllocateVA(&Allocation.Base, &Allocation.Reserve, Alignment, MEM_RESERVE,
                 PAGE_READWRITE, &Environment) < 0)
    return nullptr;
  Allocation.didReserve();

  size_t Commit = (Size + PageSize - 1) & -PageSize;
  void **OwnerSlot = nullptr;
  unsigned VAFlags = 0;
  const size_t QueriedSize = entry<QueryVirtualMemory>(Image, QueryVA)(
      Allocation.Base, nullptr, &OwnerSlot, &VAFlags);
  const uintptr_t RequiredMask = (VAUnit - 1) | (Alignment - 1);
  if ((reinterpret_cast<uintptr_t>(Allocation.Base) & RequiredMask) ||
      !QueriedSize || !OwnerSlot || Allocation.Reserve < Commit + PageSize)
    return nullptr;

  // Native free identifies this heap through the VA manager's owner slot.
  *OwnerSlot = Heap;
  if (!entry<CheckHeapCommitLimit>(Image, CheckCommitLimit)(Commit, Heap) ||
      AllocateVA(&Allocation.Base, &Commit, 0, MEM_COMMIT, PAGE_READWRITE,
                 &Environment) < 0)
    return nullptr;

  // The address word stores unused payload bytes in its low 16 bits. The size
  // word stores committed bytes, the guard-page bit and the reservation's
  // trailing-zero count as its alignment exponent.
  const uintptr_t Address = reinterpret_cast<uintptr_t>(Allocation.Base);
  const size_t AlignmentCode =
      static_cast<size_t>(__builtin_ctzll(Allocation.Reserve))
      << LargeNode::AlignmentShift;
  write<uintptr_t>(Node + LargeNode::Address, Address | (Commit - Size));
  write<size_t>(Node + LargeNode::Size,
                Commit | LargeNode::GuardPage | AlignmentCode);

  // Match RtlpHpLargeLockAcquire. Child links are XOR-encoded with their
  // parent; the root is XOR-encoded with the tree itself when encoding is
  // enabled.
  auto *const Lock = reinterpret_cast<SRWLOCK *>(Heap + HeapOffset::LargeLock);
  Byte *const Tree = Heap + HeapOffset::LargeTree;
  AcquireSRWLockExclusive(Lock);
  const bool Encoded = (read<uintptr_t>(Tree + 8) & 1) != 0;
  uintptr_t Parent = read<uintptr_t>(Tree);
  if (Encoded && Parent)
    Parent ^= reinterpret_cast<uintptr_t>(Tree);
  bool Right = false;
  while (Parent) {
    const auto *Current = reinterpret_cast<const Byte *>(Parent);
    const uintptr_t CurrentAddress =
        read<uintptr_t>(Current + LargeNode::Address) & LargeNode::AddressMask;
    Right = Address >= CurrentAddress;
    const uintptr_t Child = read<uintptr_t>(Current + (Right ? 8 : 0));
    if (!Child)
      break;
    Parent = Encoded ? Child ^ Parent : Child;
  }
  entry<InsertTreeNode>(Image, InsertNode)(
      Tree, reinterpret_cast<void *>(Parent), Right, Node);
  void *const Result = Allocation.release();
  ReleaseSRWLockExclusive(Lock);

  // Tree publication is locked; independent counters follow RTL's relaxed
  // atomic accounting. The node's earlier stores were private initialization.
  __atomic_fetch_add(
      reinterpret_cast<size_t *>(Heap + HeapOffset::ReservedPages),
      Allocation.Reserve / PageSize, __ATOMIC_RELAXED);
  __atomic_fetch_add(
      reinterpret_cast<size_t *>(Heap + HeapOffset::CommittedPages),
      Commit / PageSize, __ATOMIC_RELAXED);

  // Preserve RTL range-reservation events, including per-session tracing.
  const auto *PEB = reinterpret_cast<const Byte *>(__readgsqword(0x60));
  const Byte *Shared = read<const Byte *>(PEB + 0x90);
  const auto *Tracing = reinterpret_cast<const volatile Byte *>(0x7ffe0388);
  if (Shared && read<unsigned>(Shared))
    Tracing = Shared + 0x22e;
  if (*Tracing)
    entry<LogReservation>(Image, LogRangeReserve)(Heap, Result,
                                                  Allocation.Reserve);
  return Result;
}

#endif

// Owner has already matched UCRT's retained heap. Qualify private access before
// entering RTL's heap-walk protocol; fundamental alignment needs neither step.
void *allocateExtended(void *Owner, size_t Alignment, size_t Size,
                       AllocationError &Error) {
#if defined(__x86_64__)
  auto *const Heap = static_cast<Byte *>(Owner);
  if (!wincrt::isSegmentHeap(Heap)) {
    Error = AllocationError::Unavailable;
    return nullptr;
  }
  uintptr_t Image = __atomic_load_n(&NativeImage, __ATOMIC_ACQUIRE);
  if (!Image)
    Image = initializeNativeImage();
  if (Image == UnavailableImage || __readgsqword(HeapWalkContext)) {
    Error = AllocationError::Unavailable;
    return nullptr;
  }
  // LFH aligns the first block and stride of a power-of-two size class.
  // A cold or disabled bucket may use VS instead, so verify the one candidate
  // and free it if unsuitable. The page path supplies the guaranteed fallback.
  if (Alignment <= MaxCompactSize && Size <= MaxCompactSize) {
    const unsigned RequiredSize =
        static_cast<unsigned>(Size > Alignment ? Size : Alignment);
    // Extended alignment is at least 32, so clz never receives zero.
    const unsigned SizeClassShift = 32 - __builtin_clz(RequiredSize - 1);
    const size_t BlockSize = size_t{1} << SizeClassShift;
    void *const Block = HeapAlloc(Heap, 0, BlockSize);
    if (!Block) {
      Error = AllocationError::OutOfMemory;
      return nullptr;
    }
    if ((reinterpret_cast<uintptr_t>(Block) & (Alignment - 1)) == 0)
      return Block;
    if (!HeapFree(Heap, 0, Block))
      __fastfail(FAST_FAIL_HEAP_METADATA_CORRUPTION);
  }
  // Publish before reading flags. The exclusive walker sets HeapLocked,
  // flushes process write buffers, waits for markers to leave, then flushes
  // again. These compiler barriers preserve RTL's x64 ordering protocol.
  auto *const FlagsAddress =
      reinterpret_cast<unsigned *>(Heap + HeapOffset::Flags);
  setHeapWalkContext(reinterpret_cast<uintptr_t>(FlagsAddress));
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  void *Result = nullptr;
  AllocationError Failure = AllocationError::OutOfMemory;
  __try {
    unsigned Flags = __atomic_load_n(FlagsAddress, __ATOMIC_RELAXED);
    if ((Flags & HeapLocked) != 0)
      Flags = waitForHeapUnlock(Heap, Flags);
    const unsigned Features = __atomic_load_n(
        reinterpret_cast<unsigned *>(Image + HeapFeatures), __ATOMIC_RELAXED);
    // Check mutable modes while enrolled in the heap-walk protocol. Only the
    // ordinary non-executable, non-intercepted environment is qualified.
    // Keep fixed copies explicit here: Clang inhibits helper inlining in __try.
    HeapEnvironment Environment;
    __builtin_memcpy(&Environment, Heap, sizeof(Environment));
    if (Flags || (Features & ~1u) ||
        Environment.Attributes != DefaultEnvironmentAttributes ||
        Environment.Context != 0) {
      Failure = AllocationError::Unavailable;
    } else {
      unsigned SmallMaximum;
      __builtin_memcpy(&SmallMaximum,
                       Heap + HeapOffset::SmallContext +
                           ContextOffset::MaximumSize,
                       sizeof(SmallMaximum));
      const bool UseSmallContext =
          Alignment <= PageSize && Size <= SmallMaximum;
      Byte *const Context = Heap + (UseSmallContext ? HeapOffset::SmallContext
                                                    : HeapOffset::LargeContext);
      const unsigned UnitShift = UseSmallContext ? 12 : 16;
      unsigned Maximum;
      __builtin_memcpy(&Maximum, Context + ContextOffset::MaximumSize,
                       sizeof(Maximum));
      if (Context[ContextOffset::UnitShift] != UnitShift) {
        Failure = AllocationError::Unavailable;
      } else if (Alignment <= MaxSegmentAlignment && Size <= Maximum) {
        const size_t Unit = size_t{1} << UnitShift;
        const size_t Reserve = (Size + Unit - 1) & -Unit;
        Result = reinterpret_cast<AllocateSegment>(Image + SegAlloc)(
            Context, Size, Reserve, Size, 0);
      } else {
        Result = allocateLarge(Image, Heap, Alignment, Size);
      }
    }
  } __finally {
    // C++ scope cleanup under -fno-exceptions does not replace this SEH guard.
    // Always withdraw the marker; propagate corruption exceptions unchanged.
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    setHeapWalkContext(0);
  }
  // Report failure after withdrawing the marker. The public entry owns errno.
  if (!Result)
    Error = Failure;
  return Result;
#else
  Error = AllocationError::Unavailable;
  return nullptr;
#endif
}

// Both public interfaces validate alignment and handle zero size before entry.
// Failure initializes Error; success leaves it unused. No path accesses errno.
void *allocate(size_t Alignment, size_t Size, AllocationError &Error) {
  if (Size > _HEAP_MAXREQ) {
    Error = AllocationError::OutOfMemory;
    return nullptr;
  }
  void *Owner = __atomic_load_n(&AllocationHeap, __ATOMIC_RELAXED);
  if (!Owner)
    Owner = initializeAllocationHeap();
  // Check the PEB observation before dereferencing any heap metadata, then pass
  // the verified owner through both paths without fetching the PEB again.
  if (!Owner || wincrt::processHeap() != Owner) {
    Error = AllocationError::Unavailable;
    return nullptr;
  }
  // HeapAlloc forwards to RtlAllocateHeap; both Win64 heaps supply this
  // baseline alignment. Every path uses native failure semantics, including
  // bypassing UCRT malloc's optional Microsoft new-handler retry policy.
  if (Alignment <= NativeAlignment) {
    void *const Result = HeapAlloc(Owner, 0, Size);
    if (!Result)
      Error = AllocationError::OutOfMemory;
    return Result;
  }
  return allocateExtended(Owner, Alignment, Size, Error);
}

} // namespace

extern "C" void *__cdecl aligned_alloc(size_t Alignment, size_t Size) {
  if (!validAlignment(Alignment, 1)) {
    errno = alignedAllocError(AllocationError::InvalidAlignment);
    return nullptr;
  }
  // C17's final DR 460 correction permits non-multiple sizes. Our zero-size
  // policy rejects the request and reports errno, as required by POSIX.
  if (Size == 0) {
    errno = EINVAL;
    return nullptr;
  }
  AllocationError Error;
  void *const Result = allocate(Alignment, Size, Error);
  if (!Result)
    errno = alignedAllocError(Error);
  return Result;
}

extern "C" int __cdecl posix_memalign(void **Memory, size_t Alignment,
                                      size_t Size) {
  // A power of two at least sizeof(void *) is also a multiple of it on Win64.
  // Memory is a caller-provided writable pointer slot, as required by POSIX.
  if (!validAlignment(Alignment, sizeof(void *)))
    return posixMemalignError(AllocationError::InvalidAlignment);
  if (Size == 0) {
    *Memory = nullptr;
    return 0;
  }
  AllocationError Error;
  void *const Result = allocate(Alignment, Size, Error);
  if (!Result)
    return posixMemalignError(Error);
  // Publish only on success. Failure must preserve both *Memory and errno.
  *Memory = Result;
  return 0;
}
