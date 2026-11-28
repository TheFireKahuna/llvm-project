//===-- fill_windows.c - Binding that an image does for itself ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The address of a delay-loaded function is the exporting DLL's own wherever
// a program can observe it, while the DLL stays delayed. The linker gives
// each such DLL an ensure stub, which code runs before it observes one of the
// DLL's addresses while the DLL's gate still holds its thunk, and a list of
// what holds the addresses. The stub calls __llvm_delay_resolve with the list.
//
// The loader is the only writer of a read-only word: it binds every word of
// a delay descriptor's table under its own bracket and reports nothing for a
// DLL or an export that is missing, whose words keep their thunks. This file
// only writes the words of writable data, which no table covers.
//
// An image also holds words that only it can compute, which the linker lists
// with what each is computed from. The fill writes them before any other
// code of the image runs: the image's entry point, fill_entry.S's, calls it
// and then the entry that the image was linked with, or an executable whose
// thread-local template holds such a word runs it from its first TLS
// callback. The words of read-only data are read-only again when it returns.
// Each thread copies a thread-local word before the fill writes it, so the
// fill also writes it in every thread that holds the template's value from
// before: its own, every other of a DLL loaded at run time, and each that
// attaches to the DLL later. It also binds by name
// what a DLL imports from the executable that loads it, in the running
// executable, which the loader would look for by its file's name, and the
// weak imports, which are null where their DLL is not loaded or lacks them.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#if defined(__x86_64__)
#define NTAPI __attribute__((ms_abi))
#else
#define NTAPI
#endif

extern char __ImageBase[];

// Exported by ntdll; no SDK header declares it.
__declspec(dllimport) void *NTAPI LdrResolveDelayLoadedAPI(
    void *ParentModuleBase, const void *DelayloadDescriptor,
    void *FailureDllHook, void *FailureSystemHook, void *ThunkAddress,
    uint32_t Flags);

// The list, as the linker writes it, with every address relative to the
// image base: Tables descriptors to resolve in order, the DLL's own and then
// its runs, each with the first word of its table and the number of its
// imports; Words words of writable data, each with the function's entry of
// the DLL's table and the thunk the word holds; and the gate's descriptor.
struct DelayTable {
  uint32_t Descriptor, Slot, Count;
};
struct DelayWord {
  uint32_t Word, Cell, Thunk;
};
struct DelayList {
  uint32_t Tables, Words;
};

// The address of a word, which the linker lists only where it is aligned.
static uintptr_t *wordAt(uint32_t Rva) {
  return (uintptr_t *)((uintptr_t)__ImageBase + Rva);
}

// The loader takes a word for unbound exactly when it points into the image.
static int isUnbound(const uintptr_t *Slot, uint32_t ImageSize) {
  return __atomic_load_n(Slot, __ATOMIC_RELAXED) - (uintptr_t)__ImageBase <
         ImageSize;
}

// Has the loader bind a descriptor's table. The loader does nothing for a
// word that is bound, so it is given the first that is not; and where it
// cannot allocate, it binds at most 128 words from the one it is given, so
// it is asked once for each 128 that hold an unbound word.
static void resolveTable(const struct DelayTable *Table, uint32_t ImageSize,
                         uint32_t Flags) {
  uintptr_t *Slot = wordAt(Table->Slot);
  for (uint32_t I = 0; I < Table->Count; ++I)
    if (isUnbound(&Slot[I], ImageSize)) {
      LdrResolveDelayLoadedAPI(__ImageBase, __ImageBase + Table->Descriptor, 0,
                               0, &Slot[I], Flags);
      I |= 127;
    }
}

// Binds what List names: the tables; then each writable word that still
// holds its thunk, to the address that the loader bound the function's entry
// to, so that a value the program stored in the word stays; and last the
// gate, so that a thread that finds the gate bound finds every word written.
// A word whose function the DLL does not export keeps its thunk, and where
// the DLL does not load so does the gate, and a later call binds them.
void __llvm_delay_resolve(const struct DelayList *List) {
  // The image's headers: the size of the image, and from the load
  // configuration the search path of /dependentloadflag, of which the loader
  // admits these flags for a delayed load, so that this load finds the DLL
  // that a call of one of its functions does.
  const char *Optional =
      __ImageBase + *(const int32_t *)(__ImageBase + 0x3c) + 24;
  uint32_t ImageSize = *(const uint32_t *)(Optional + 56);
  uint32_t Config = *(const uint32_t *)(Optional + 112 + 10 * 8);
  uint32_t Flags = 0;
  if (Config && *(const uint32_t *)(__ImageBase + Config) >= 80)
    Flags = *(const uint16_t *)(__ImageBase + Config + 78) & 0x7b00;

  const struct DelayTable *Table = (const struct DelayTable *)(List + 1);
  const struct DelayWord *Word =
      (const struct DelayWord *)(Table + List->Tables);
  const struct DelayTable *Gate =
      (const struct DelayTable *)(Word + List->Words);
  for (; (const void *)Table != (const void *)Word; ++Table)
    resolveTable(Table, ImageSize, Flags);
  for (; (const void *)Word != (const void *)Gate; ++Word) {
    const uintptr_t *Cell = wordAt(Word->Cell);
    uintptr_t Thunk = (uintptr_t)__ImageBase + Word->Thunk;
    if (!isUnbound(Cell, ImageSize))
      __atomic_compare_exchange_n(wordAt(Word->Word), &Thunk,
                                  __atomic_load_n(Cell, __ATOMIC_RELAXED), 0,
                                  __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
  resolveTable(Gate, ImageSize, Flags);
}

// The fill's list, as the linker writes it, with every address relative to
// the image base: the header, then the ranges and the words, of which the
// first are cells, the vague ones first, and the last thread-local. A range is
// read-only memory that holds words the fill writes where they are, and Sealed
// the read-only section that the linker gives the others, which begins with a
// word that is set once the fill has written. The header ends with the entry
// of a DLL's offer table that names the DLL itself, or zero, and the number of
// cells that are name words, which follow the other vague cells.
struct FillRange {
  uint32_t Rva, Size;
};
struct FillList {
  uint32_t Ranges, Cells, Words, ThreadLocalWords;
  struct FillRange Sealed;
  uint32_t Self, Names;
};
// A word, and the entry of an import's address table that it is computed
// from, which is 8-aligned, with the kind of the word in its low three bits:
// what the entry holds plus Addend; the word at the address that the entry
// holds; what the function that the entry holds returns for the image's
// base, which is zero where the image cannot run; or for an import that the
// fill binds by name, the import's pointer, with no entry, and in Addend its
// name and above that where it is: 0 for the executable that loads the
// image, 1 for a weak import from it, or else a weak import's DLL, by its
// name in UTF-16; or for a cell, a word that holds the address of an
// overridable default, with no entry, its use in the three bits above the
// kind and its slot in the executable's override table above bit 8, and in
// Addend the RVA of the image's own default where the word is the image's
// own, or of a vague entity, with the top 24 bits of its key above bit 8, the
// RVA of the image's own offer entry for it in the low half of Addend and,
// where the word is the image's own, the RVA of the image's instance in the
// high half, or for the name word of a type_info object that the image keeps,
// the RVA of the name that it holds; or for a thread-local variable's offset
// from the thread pointer, the place that the record that the entry points at
// names, plus the record's word at Addend.
struct FillWord {
  uint32_t Word, Source;
  int64_t Addend;
};
enum { WordAddress, WordValue, WordInit, WordImport, WordCell, WordTlsOffset };
// Where the table names a cell's slot, a call cell takes what a call of the
// default reaches, an address cell the default's address in every image, and
// a default's check cell what its check transfers to. A vague cell takes the
// instance that the executable's offer table names, and a name word that
// instance's name word.
enum { CellCall, CellAddress, CellCheck, CellVague, CellName };
// An offer table's entry for a function, one that names a word holding the
// instance's address, and one of an instance that its DLL added since a
// release that an older image may have been linked against; the bits of the
// word at 16 of an entry that two copies of one entity agree on: the size,
// the function, guard, image and type_info bits of the flags, and the
// alignment; and the table's flag that it has an entry of a type_info object.
enum {
  OfferFunction = 0x40,
  OfferIndirect = 0x400,
  OfferAddition = 0x1000,
  OfferTableDescriptors = 1
};
static const uint64_t OfferShape = 0x00FF2848FFFFFFFF;

enum { PageReadOnly = 2, PageReadWrite = 4, MaxChanges = 256 };
// A word that the fill wrote and read-only memory no longer holds.
#define STATUS_INVALID_IMAGE_HASH ((int32_t)0xC0000428)
#define STATUS_DLL_INIT_FAILED ((int32_t)0xC0000142)
// An import that no image exports.
#define STATUS_ENTRYPOINT_NOT_FOUND ((int32_t)0xC0000139)

// One walk of the list. The first computes every word, stores each word of
// writable memory, and collects the read-only words that do not hold their
// values yet, as many as Rva and Value have room for; the checking one
// computes every word again and fails where a read-only word differs. Inits
// says that a walk calls the functions that words are computed with, which
// the first round leaves to the next. Loaded says whether a DLL searches, and
// is negative until a walk needs to know; the search's images, at Scope, are
// found once, Count of them and from MaxScope up to Tail the others that an
// entity's search reads, each of which is negative until then. Collided says
// that an offer table had an entry of another name under an entity's key,
// and First that the fill has not written the image before.
struct Pass {
  const struct FillList *List;
  int Check, Full, Loaded, Inits, Collided, First;
  int32_t Status;
  uint32_t Changes;
  int32_t Count, Tail;
  const char **Scope;
  uint32_t Rva[MaxChanges];
  uint64_t Value[MaxChanges];
};

// A word of the image, which the linker lists wherever a relocation is.
typedef uint64_t __attribute__((aligned(1))) Unaligned;

__declspec(dllimport) int32_t NTAPI
NtProtectVirtualMemory(void *Process, void **Base, uintptr_t *Size,
                       uint32_t Protection, uint32_t *OldProtection);
__declspec(dllimport) int32_t NTAPI
NtQueryVirtualMemory(void *Process, const void *Base, uint32_t Class,
                     void *Information, uintptr_t Size, uintptr_t *ReturnSize);
struct AnsiString {
  uint16_t Length, MaximumLength;
  const char *Buffer;
};
__declspec(dllimport) int32_t NTAPI LdrGetProcedureAddressForCaller(
    const void *Module, const struct AnsiString *Name, uint32_t Ordinal,
    void **Address, uint32_t Flags, void *Caller);
struct UnicodeString {
  uint16_t Length, MaximumLength;
  const uint16_t *Buffer;
};
__declspec(dllimport) int32_t NTAPI LdrGetDllHandleEx(
    uint32_t Flags, const uint16_t *Path, uint32_t *Characteristics,
    const struct UnicodeString *Name, void **Module);
__declspec(dllimport) int32_t NTAPI LdrAddRefDll(uint32_t Flags,
                                                 const void *Module);
__declspec(dllimport) void NTAPI RtlExitUserProcess(int32_t Status);

// What the fill calls to write the thread-local words of other threads.
struct ClientId {
  uintptr_t Process, Thread;
};
struct ObjectAttributes {
  uint32_t Length;
  void *Root, *Name;
  uint32_t Attributes;
  void *Descriptor, *Qos;
};
__declspec(dllimport) int32_t NTAPI NtQuerySystemInformation(
    uint32_t Class, void *Information, uint32_t Length, uint32_t *Returned);
__declspec(dllimport) int32_t NTAPI
NtAllocateVirtualMemory(void *Process, void **Base, uintptr_t ZeroBits,
                        uintptr_t *Size, uint32_t Type, uint32_t Protection);
__declspec(dllimport) int32_t NTAPI NtFreeVirtualMemory(void *Process,
                                                        void **Base,
                                                        uintptr_t *Size,
                                                        uint32_t Type);
__declspec(dllimport) int32_t NTAPI NtOpenThread(
    void **Thread, uint32_t Access, const struct ObjectAttributes *Attributes,
    const struct ClientId *Id);
__declspec(dllimport) int32_t NTAPI NtGetNextThread(void *Process, void *Thread,
                                                    uint32_t Access,
                                                    uint32_t Attributes,
                                                    uint32_t Flags,
                                                    void **Next);
__declspec(dllimport) int32_t NTAPI
NtQueryInformationThread(void *Thread, uint32_t Class, void *Information,
                         uint32_t Length, uint32_t *Returned);
__declspec(dllimport) int32_t NTAPI NtSuspendThread(void *Thread,
                                                    uint32_t *Count);
__declspec(dllimport) int32_t NTAPI NtResumeThread(void *Thread,
                                                   uint32_t *Count);
__declspec(dllimport) int32_t NTAPI NtGetContextThread(void *Thread,
                                                       void *Context);
__declspec(dllimport) int32_t NTAPI NtClose(void *Handle);
__declspec(dllimport) int32_t NTAPI NtOpenThreadToken(void *Thread,
                                                      uint32_t Access,
                                                      uint8_t AsSelf,
                                                      void **Token);
__declspec(dllimport) int32_t NTAPI NtSetInformationThread(
    void *Thread, uint32_t Class, const void *Information, uint32_t Length);
__declspec(dllimport) int32_t NTAPI NtTerminateProcess(void *Process,
                                                       int32_t Status);

extern const struct FillList __llvm_fill_list
    __attribute__((visibility("hidden")));

// The fill runs before the image's security cookie is set, and every pointer
// it calls through is the loader's or the linker's, in read-only memory.
#pragma clang attribute push(                                                  \
    __attribute__((no_stack_protector, no_sanitize("kcfi"), guard(nocf))),     \
    apply_to = function)

// The sealed section, and then each range.
static const struct FillRange *rangeAt(const struct FillList *L, uint32_t I) {
  return I ? (const struct FillRange *)(L + 1) + I - 1 : &L->Sealed;
}

static int isReadOnly(const struct FillList *L, uint32_t Rva) {
  for (uint32_t I = 0; I <= L->Ranges; ++I)
    if (Rva - rangeAt(L, I)->Rva < rangeAt(L, I)->Size)
      return 1;
  return 0;
}

// Gives the read-only memory that the list names a protection. Opening it
// stops at the first range that stays read-only; sealing it tries every
// range, and fails where one stays writable. The calls are made here, so
// that the image holds no function that changes a protection.
static inline __attribute__((always_inline)) int32_t
protect(const struct FillList *L, uint32_t Protection) {
  int32_t Result = 0;
  for (uint32_t I = 0; I <= L->Ranges; ++I) {
    void *Base = __ImageBase + rangeAt(L, I)->Rva;
    uintptr_t Size = rangeAt(L, I)->Size;
    uint32_t Old;
    int32_t Status =
        NtProtectVirtualMemory((void *)-1, &Base, &Size, Protection, &Old);
    if (Status < 0 && Protection == PageReadWrite)
      return Status;
    if (Status < 0)
      Result = Status;
  }
  return Result;
}

// Takes the value of the word at Rva. A check compares only read-only
// memory: a writable word is the program's to change.
static void put(struct Pass *P, uint32_t Rva, uint64_t Value) {
  Unaligned *Word = (Unaligned *)(__ImageBase + Rva);
  if (*Word == Value)
    return;
  if (!isReadOnly(P->List, Rva)) {
    if (!P->Check)
      *Word = Value;
  } else if (P->Check) {
    P->Status = STATUS_INVALID_IMAGE_HASH;
  } else if (P->Changes == MaxChanges) {
    P->Full = 1;
  } else {
    P->Rva[P->Changes] = Rva;
    P->Value[P->Changes++] = Value;
  }
}

// The running thread's environment block.
static char *teb(void) {
  char *Teb;
#if defined(__x86_64__)
  __asm__("movq %%gs:0x30, %0" : "=r"(Teb));
#else
  __asm__("mov %0, x18" : "=r"(Teb));
#endif
  return Teb;
}

// A thread-local word holds this until the fill writes it, which no pointer
// holds, so that a thread's own store is never taken for it.
static const uint64_t Unfilled = 0xF1F1F1F1F1F1F1F1;

// Writes each thread-local word of the copy of the image's thread-local data
// in the thread whose environment block is Teb that still holds Unfilled with
// the template's. The block names the thread's array of copies, which the
// image's TLS directory states the index into and its template the start of
// each; a thread that has no array has copied nothing.
static void fillCopy(const struct FillList *L, const char *Teb) {
  const char *Optional =
      __ImageBase + *(const int32_t *)(__ImageBase + 0x3c) + 24;
  const uintptr_t *Directory =
      (const uintptr_t *)(__ImageBase +
                          *(const uint32_t *)(Optional + 112 + 9 * 8));
  char *const *Copies = *(char *const *const *)(Teb + 0x58);
  char *Copy = Copies && (const char *)Copies != Teb + 0x58
                   ? Copies[*(const uint32_t *)Directory[2]]
                   : 0;
  if (!Copy)
    return;
  // The copy less the template's address in the image, at which a word's
  // RVA is its copy.
  Copy -= Directory[0] - (uintptr_t)__ImageBase;
  const struct FillWord *W =
      (const struct FillWord *)((const char *)(L + 1) + 8 * L->Ranges) +
      L->Words - L->ThreadLocalWords;
  for (uint32_t I = 0; I != L->ThreadLocalWords; ++I)
    if (*(Unaligned *)(Copy + W[I].Word) == Unfilled)
      *(Unaligned *)(Copy + W[I].Word) =
          *(const Unaligned *)(__ImageBase + W[I].Word);
}

// What the word at Rva holds once the fill has written: a read-only word that
// this pass has computed already takes its new value.
static uint64_t valueAt(const struct Pass *P, uint32_t Rva) {
  for (uint32_t I = P->Changes; I--;)
    if (P->Rva[I] == Rva)
      return P->Value[I];
  return *(const Unaligned *)(__ImageBase + Rva);
}

// The process's environment block.
static const char *peb(void) {
  const char *Peb;
#if defined(__x86_64__)
  __asm__("movq %%gs:0x60, %0" : "=r"(Peb));
#else
  __asm__("ldr %0, [x18, #0x60]" : "=r"(Peb));
#endif
  return Peb;
}

// The optional header of the image at Base.
static const char *headers(const char *Base) {
  return Base + *(const uint32_t *)(Base + 0x3c) + 24;
}

// The size of the image at Base, from its headers, or zero where they do not
// fit its first page.
static uint32_t imageSize(const char *Base) {
  return *(const uint32_t *)(Base + 0x3c) <= 4096 - 264
             ? *(const uint32_t *)(headers(Base) + 56)
             : 0;
}

// Whether the string at S, of which Size bytes are in the image, is Prefix
// followed by Name, or sorts before or after it.
static int compareName(const char *S, uint32_t Size, const char *Prefix,
                       const char *Name) {
  for (; *Prefix; --Size, ++S, ++Prefix)
    if (!Size || *S != *Prefix)
      return Size ? (unsigned char)*S - (unsigned char)*Prefix : 1;
  for (; Size; --Size, ++S, ++Name)
    if (*S != *Name || !*S)
      return (unsigned char)*S - (unsigned char)*Name;
  return 1;
}

// Whether the string at S, of which Size bytes are in its image, is the one
// at Own, of which OwnSize bytes are in this image: a word at a time while
// both have one, up to the first NUL of the word that holds it.
static __attribute__((noinline)) int
sameName(const char *S, uint32_t Size, const char *Own, uint32_t OwnSize) {
  for (; Size >= 8 && OwnSize >= 8; S += 8, Own += 8, Size -= 8, OwnSize -= 8) {
    uint64_t Word = *(const Unaligned *)S;
    uint64_t Differ = Word ^ *(const Unaligned *)Own;
    uint64_t Nul = (Word - 0x0101010101010101) & ~Word & 0x8080808080808080;
    if (Nul)
      return !(Differ & (Nul ^ (Nul - 1)));
    if (Differ)
      return 0;
  }
  return !compareName(S, Size, "", Own);
}

// The address that the image at Base, of Size bytes, exports as Prefix
// followed by Name, or zero. Its export names are searched where they are,
// reading nothing outside the image, and only a name it has is asked of the
// loader, by the image's own string, which follows a forwarder and grants the
// address to control flow guard as it does for GetProcAddress. The image's
// own address is the caller's.
static uint64_t exportOf(const char *Base, uint32_t Size, const char *Prefix,
                         const char *Name) {
  uint32_t Rva = *(const uint32_t *)(headers(Base) + 112);
  if (Rva >= Size || Size - Rva < 40)
    return 0;
  uint32_t Count = *(const uint32_t *)(Base + Rva + 24);
  uint32_t Names = *(const uint32_t *)(Base + Rva + 32);
  if (Names > Size || (Size - Names) / 4 < Count)
    return 0;
  for (uint32_t Low = 0, High = Count; Low < High;) {
    uint32_t Mid = Low + (High - Low) / 2;
    uint32_t At = ((const uint32_t *)(Base + Names))[Mid];
    if (At >= Size)
      return 0;
    int Order = compareName(Base + At, Size - At, Prefix, Name);
    if (Order < 0) {
      Low = Mid + 1;
    } else if (Order > 0) {
      High = Mid;
    } else {
      uint16_t Length = 0;
      while (Base[At + Length])
        ++Length;
      struct AnsiString String = {Length, (uint16_t)(Length + 1), Base + At};
      void *Address;
      return LdrGetProcedureAddressForCaller(Base, &String, 0, &Address, 1,
                                             __ImageBase) < 0
                 ? 0
                 : (uint64_t)Address;
    }
  }
  return 0;
}

// The image that the kernel maps at Address, by the base of its mapping, or
// null.
static __attribute__((noinline)) const char *imageAt(const void *Address) {
  struct {
    const void *Base;
    const char *AllocationBase;
    uint32_t AllocationProtect;
    uintptr_t RegionSize;
    uint32_t State, Protect, Type;
  } Region;
  return NtQueryVirtualMemory((void *)-1, Address, 0, &Region, sizeof(Region),
                              0) < 0 ||
                 Region.Type != 0x1000000 || !imageSize(Region.AllocationBase)
             ? 0
             : Region.AllocationBase;
}

// The executable that the process runs, whatever its file name, as the
// kernel maps it: an image whose headers are not a DLL's. Null where its
// environment block names anything else.
static const char *executable(void) {
  const char *Base = *(const char *const *)(peb() + 0x10);
  return imageAt(Base) == Base &&
                 !(*(const uint16_t *)(headers(Base) - 2) & 0x2000)
             ? Base
             : 0;
}

// What control flow guard knows of a call that reaches Rva in the image at
// Base, of Size bytes: -1 where its load configuration lists the functions
// that a call may reach and Rva is not one of them, and otherwise the flags
// of its entry, of which 2 alone says that the loader must grant the export.
static int guardOf(const char *Base, uint32_t Size, uint32_t Rva) {
  uint32_t Config = *(const uint32_t *)(headers(Base) + 112 + 10 * 8);
  if (!Config || Config > Size - 0x94 ||
      *(const uint32_t *)(Base + Config) < 0x94 ||
      !(*(const uint32_t *)(Base + Config + 0x90) & 0x400))
    return 0;
  uint32_t Stride = 4 + (*(const uint32_t *)(Base + Config + 0x90) >> 28);
  uint64_t Table = *(const uint64_t *)(Base + Config + 0x80) - (uint64_t)Base;
  uint64_t Count = *(const uint64_t *)(Base + Config + 0x88);
  if (Table > Size || (Size - Table) / Stride < Count)
    return -1;
  for (uint64_t Low = 0, High = Count; Low < High;) {
    uint64_t Mid = Low + (High - Low) / 2;
    const char *At = Base + Table + Mid * Stride;
    if (*(const uint32_t *)At == Rva)
      return Stride > 4 ? (uint8_t)At[4] : 0;
    if (*(const uint32_t *)At < Rva)
      Low = Mid + 1;
    else
      High = Mid;
  }
  return -1;
}

// The offer table of the image at Base, of Size bytes, where this toolchain
// made the image: the word at 0x38 of its header names the table, which opens
// with this and lies in the image with its buckets, as every entry does.
static const uint32_t *offerTable(const char *Base, uint32_t Size) {
  uint32_t Rva = *(const uint32_t *)(Base + 0x38);
  const uint32_t *H = (const uint32_t *)(Base + Rva);
  return Rva && Rva % 8 == 0 && Size >= 16 && Rva <= Size - 16 &&
                 H[0] == 0x4F7A1D2B && H[1] <= 24 &&
                 (Size - Rva - 16) / 24 >= H[2] &&
                 (Size - Rva - 16 - 24 * H[2]) / 4 > (1u << H[1])
             ? H
             : 0;
}

// An offer table as a probe reads it: the image at Base, of Size bytes, and
// the table's Count entries, in the order of their keys, of 24 bytes each,
// and its buckets, which index them by the top 24 bits of a key shifted right
// by Shift. An entry is the key, the RVA of the name, the RVA of the instance
// or of a word that holds its address, as its flags say, the size as 32 bits,
// the flags as 16 and the logarithm of the alignment. A guard's states its
// variable's shape, so that the two bind together.
struct Offers {
  const char *Base, *Entries;
  const uint32_t *Buckets;
  uint32_t Size, Count, Shift;
};

// The table H of the image at Base, of Size bytes, as a probe reads it, with
// no entries where H is null.
static struct Offers offersOf(const char *Base, uint32_t Size,
                              const uint32_t *H) {
  static const uint32_t None[2];
  struct Offers O = {Base, 0, None, Size, 0, 24};
  if (H) {
    O.Entries = (const char *)(H + 4);
    O.Buckets = (const uint32_t *)(O.Entries + 24 * H[2]);
    O.Count = H[2];
    O.Shift = 24 - H[1];
  }
  return O;
}

// Whether the offer table O has an entry of a type_info object, which its
// header's last word says.
static inline int hasDescriptors(const struct Offers *O) {
  return O->Count && ((const uint32_t *)O->Entries)[-1] & OfferTableDescriptors;
}

// The entry of the offer table O for the entity whose name has Key, or null;
// with Top alone, the top 24 bits of Key, an entry that may be it.
static inline const char *offerEntry(const struct Offers *O, uint64_t Key,
                                     int Top) {
  const uint32_t *Bucket = O->Buckets + ((Top ? Key : Key >> 40) >> O->Shift);
  for (uint32_t I = Bucket[0]; I < Bucket[1] && I < O->Count; ++I)
    if (*(const uint64_t *)(O->Entries + 24 * I) >> (Top ? 40 : 0) == Key)
      return O->Entries + 24 * I;
  return 0;
}

// The address of the instance that the offer table O names for the entity
// of this image's own entry Own, or zero, of which E is the entry where it
// is not null and has Own's key. The two entries state one entity only where
// their names are equal and so are the size, the alignment and the flags of the
// shape, whether it is a function and whether a guard initializes it: an image
// that keeps another version of the entity keeps its own instance. The names
// are compared unless the instance is Now, what the word already holds: no two
// entities have one address, and binding the word to it changes nothing.
// What the word holds is trusted only until a pass meets two names with one
// key, after which a word that a write reached while it was writable could
// hold the other's address.
static inline uint64_t offerOf(struct Pass *P, const struct Offers *O,
                               const char *Own, uint64_t Now, const char *E) {
  const char *Base = O->Base;
  uint32_t Size = O->Size;
  if (!E || *(const uint64_t *)E != *(const uint64_t *)Own)
    E = offerEntry(O, *(const uint64_t *)Own, 0);
  uint32_t Name = E ? *(const uint32_t *)(E + 8) : 0;
  uint32_t Rva = E ? *(const uint32_t *)(E + 12) : 0;
  uint32_t OwnName = *(const uint32_t *)(Own + 8);
  if (!E || Rva > Size - 8 || Name >= Size ||
      (*(const uint64_t *)(E + 16) ^ *(const uint64_t *)(Own + 16)) &
          OfferShape)
    return 0;
  uint64_t Value = *(const uint16_t *)(E + 20) & OfferIndirect
                       ? *(const uint64_t *)(Base + Rva)
                       : (uint64_t)(Base + Rva);
  if ((Value == Now && !P->Collided) ||
      sameName(Base + Name, Size - Name, __ImageBase + OwnName,
               imageSize(__ImageBase) - OwnName))
    return Value;
  P->Collided = 1;
  return 0;
}

// What a name word of the image holds until the fill binds it: the address of
// the name at Rva, with the non-unique bit and in bits 48 to 62 the length of
// the blob that it names, the name, its NUL and the layout shape after it,
// which the word's own entry Own states as its size, or 0x7FFF where that
// does not fit them.
static uint64_t nameWord(const char *Own, uint32_t Rva) {
  uint32_t Size = *(const uint32_t *)(Own + 16);
  return ((uint64_t)__ImageBase + Rva) |
         (uint64_t)(Size < 0x7FFF ? Size : 0x7FFF) << 48 | 1ull << 63;
}

// The name word of the type_info object at Value, which an offer table names
// for the entity of the image's own entry Own, where it names the blob of
// Mine, of the length that Own states: the same bytes, where a saturated
// length is the word ahead of them. Otherwise zero: a class of another layout
// under that name keeps its own word and compares unequal.
static uint64_t nameOf(uint64_t Value, uint64_t Mine, const char *Own) {
  uint64_t Theirs = *(const Unaligned *)(Value + 8);
  uint32_t Size = *(const uint32_t *)(Own + 16);
  const char *A = (const char *)(Mine & 0xFFFFFFFFFFFF);
  const char *B = (const char *)(Theirs & 0xFFFFFFFFFFFF);
  if (Theirs == Mine)
    return Theirs;
  if ((Theirs ^ Mine) >> 48 ||
      (Mine >> 48 == 0xFFFF && *(const Unaligned *)(B - 8) != Size))
    return 0;
  for (; Size >= 8; A += 8, B += 8, Size -= 8)
    if (*(const Unaligned *)A != *(const Unaligned *)B)
      return 0;
  for (; Size; --Size)
    if (*A++ != *B++)
      return 0;
  return Theirs;
}

enum { MaxScope = 128 };

// The image of the loader's entry M where this toolchain made it and the
// process loaded it as it started, or with Loaded where its initialization
// has begun and its detach has not, which this image's has not, or null.
// Each entry of the list has its links first, those of the order of
// initialization at 0x20, which the loader sets before it calls the entry
// point and leaves at 1 before the detach, the base at 0x30, the size at
// 0x40, the base name at 0x58 and the flags at 0x68, of which 0x20 marks a
// module that the process loaded as it started.
static const char *markedImage(const char *M, int Loaded) {
  const char *Base = *(const char *const *)(M + 0x30);
  if (Loaded ? *(const uintptr_t *)(M + 0x20) <= 1 || Base == __ImageBase
             : !(*(const uint32_t *)(M + 0x68) & 0x20))
    return 0;
  uint32_t Size = imageSize(Base);
  return Size && Size == *(const uint32_t *)(M + 0x40) &&
                 offerTable(Base, Size)
             ? Base
             : 0;
}

// The initial image whose file the loader's list names as the NUL-terminated
// Name does, of which Limit bytes are in its image, ignoring the case of
// ASCII letters.
static const char *scopeImage(const char *Head, const char *Name,
                              uint32_t Limit) {
  for (const char *M = *(const char *const *)Head; M != Head;
       M = *(const char *const *)M) {
    const char *Base = markedImage(M, 0);
    if (!Base)
      continue;
    const uint16_t *File = *(const uint16_t *const *)(M + 0x60);
    uint32_t Length = *(const uint16_t *)(M + 0x58) / 2, K = 0;
    for (; K != Length && K != Limit && Name[K]; ++K)
      if ((File[K] | 0x20) != (Name[K] | 0x20) ||
          ((File[K] | 0x20) - 'a' >= 26 && File[K] != (uint8_t)Name[K]))
        break;
    if (K == Length && K != Limit && !Name[K])
      return Base;
  }
  return 0;
}

// The images whose exports bind an import from the executable that loads
// the image, in the order of the search: the executable, then breadth-first
// over the imports of each image of the list, those that the process loaded
// as it started and this toolchain made. A system or a foreign DLL exports
// plain C names that are not the program's, and is not followed. Returns how
// many, or one more than Scope holds where it has no room for them all.
static __attribute__((noinline)) uint32_t hostScope(const char *Executable,
                                                    const char **Scope) {
  const char *Head = *(const char *const *)(peb() + 0x18) + 0x10;
  uint32_t Count = 1;
  Scope[0] = Executable;
  for (uint32_t I = 0; I != Count; ++I) {
    const char *Base = Scope[I];
    uint32_t Size = imageSize(Base);
    uint32_t At = *(const uint32_t *)(headers(Base) + 120);
    for (; At && At <= Size - 20 && *(const uint32_t *)(Base + At + 12);
         At += 20) {
      uint32_t Name = *(const uint32_t *)(Base + At + 12);
      const char *Image =
          Name < Size ? scopeImage(Head, Base + Name, Size - Name) : 0;
      uint32_t J = 0;
      while (J != Count && Scope[J] != Image)
        ++J;
      if (!Image || J != Count)
        continue;
      if (Count == MaxScope)
        return MaxScope + 1;
      Scope[Count++] = Image;
    }
  }
  return Count;
}

// The search's images, which the fill finds once; none where the executable
// is not what the kernel mapped.
static uint32_t scopeOf(struct Pass *P) {
  if (P->Count < 0) {
    const char *Executable = executable();
    P->Count = Executable ? hostScope(Executable, P->Scope) : 0;
  }
  return P->Count;
}

// Has the loader grant a call of Value, a function that the image at Base,
// of Size bytes, exports and never takes the address of, which control flow
// guard refuses until then: the loader does so as it finds the export by the
// name of the image's own entry Own. An export of that name that is another
// address is the image's failure.
static __attribute__((noinline)) void grant(struct Pass *P, const char *Base,
                                            uint32_t Size, uint64_t Value,
                                            const char *Own) {
  if ((guardOf(Base, Size, Value - (uint64_t)Base) & 3) == 2 &&
      exportOf(Base, Size, "", __ImageBase + *(const uint32_t *)(Own + 8)) !=
          Value)
    P->Status = STATUS_INVALID_IMAGE_HASH;
}

// Whether the initialization of the module at Base has begun, which the
// loader's entry for it says as markedImage reads it.
static __attribute__((noinline)) int begun(const char *Base) {
  const char *Head = *(const char *const *)(peb() + 0x18) + 0x10;
  for (const char *M = *(const char *const *)Head; M != Head;
       M = *(const char *const *)M)
    if (*(const char *const *)(M + 0x30) == Base)
      return *(const uintptr_t *)(M + 0x20) > 1;
  return 0;
}

// What the image at Base, whose offer table the search checked as it took
// the image, binds Name to, or with no Name the entity of the image's own
// entry Own, whose word holds Now. An instance that its DLL added since its
// baseline is not settled while that DLL's fill, which may yield it to
// another, has not run, which this image's has not.
static uint64_t bindIn(struct Pass *P, const char *Base, const char *Name,
                       const char *Own, uint64_t Now) {
  if (Name)
    return exportOf(Base, imageSize(Base), "", Name);
  struct Offers O = offersOf(
      Base, 0, (const uint32_t *)(Base + *(const uint32_t *)(Base + 0x38)));
  const char *E = offerEntry(&O, *(const uint64_t *)Own, 0);
  if (!E)
    return 0;
  O.Size = imageSize(Base);
  uint64_t Value = offerOf(P, &O, Own, Now, E);
  if (Value && (*(const uint16_t *)(E + 20) & OfferAddition) &&
      (Base == __ImageBase || !begun(Base)))
    return 0;
  if (!P->Check && Value - (uint64_t)Base < O.Size &&
      *(const uint16_t *)(Own + 20) & OfferFunction)
    grant(P, Base, O.Size, Value, Own);
  return Value;
}

// The instance of an entity that the image added since its baseline, whose
// own entry Own says so, in an image that this toolchain did not make, which
// exports it by its mangled name with no size, alignment or initialization to
// refuse it on: the first in the order of loading, the executable first. A
// variable that a guard initializes binds only where its guard is exported
// too, and a guard only with its variable, which has a mangled name. An image
// that the process did not load as it started is pinned.
static __attribute__((noinline)) uint64_t foreign(struct Pass *P,
                                                  const char *Own) {
  const char *Name = __ImageBase + *(const uint32_t *)(Own + 8);
  uint16_t Flags = *(const uint16_t *)(Own + 20);
  int Guarded = (Flags >> 2 & 6) == 2, Guard = Name[2] == 'G' && Name[3] == 'V';
  if (Name[0] != '_' || Name[1] != 'Z' ||
      (Guarded && Guard && (unsigned)(Name[4] - '0') < 10))
    return 0;
  const char *Head = *(const char *const *)(peb() + 0x18) + 0x10;
  for (const char *M = *(const char *const *)Head; M != Head;
       M = *(const char *const *)M) {
    const char *Base = *(const char *const *)(M + 0x30);
    uint32_t Size = imageSize(Base);
    uint64_t Address;
    if (Base == __ImageBase || !Size || Size != *(const uint32_t *)(M + 0x40) ||
        offerTable(Base, Size) || !(Address = exportOf(Base, Size, "", Name)) ||
        (Guarded &&
         !exportOf(Base, Size, Guard ? "_Z" : "_ZGV", Name + (Guard ? 4 : 2))))
      continue;
    if (!P->Check && !(*(const uint32_t *)(M + 0x68) & 0x20) &&
        LdrAddRefDll(1, Base) < 0)
      P->Status = STATUS_DLL_INIT_FAILED;
    return Address;
  }
  return 0;
}

// The address of an import from the executable that loads the image, or of
// an instance of the entity of the image's own entry Own, from the first
// image of the search that has it; an entity's search follows the probe of
// the executable's table, which it does not read again. After Scope, the
// search goes on over the other images in the order of their loading: for an
// import the initial ones that only a system or a foreign DLL imports or that
// Scope has no room for, and for an entity every one whose initialization has
// begun, which the first walk pins once this image binds to it, so that the
// instance lives as long as the process, and then for an addition the images
// of other producers. A pin that the loader refuses fails the image.
static uint64_t search(struct Pass *P, const char *Name, const char *Own,
                       uint64_t Now) {
  uint32_t Count = scopeOf(P);
  const char **Scope = P->Scope;
  // No search runs where the executable is not what the kernel mapped.
  if (!Count)
    return 0;
  uint64_t Address = 0;
  uint32_t Held = Count > MaxScope ? MaxScope : Count;
  for (uint32_t I = !Name; !Address && I != Held; ++I)
    Address = bindIn(P, Scope[I], Name, Own, Now);
  const char *Head = *(const char *const *)(peb() + 0x18) + 0x10;
  // An entity's other images are found once, and where Scope has no room for
  // them all the search goes on over the list.
  if (!Name && P->Tail < 0) {
    P->Tail = MaxScope;
    for (const char *M = *(const char *const *)Head;
         M != Head && P->Tail <= 2 * MaxScope; M = *(const char *const *)M) {
      const char *Base = markedImage(M, 1);
      uint32_t J = 0;
      while (J != Held && Scope[J] != Base)
        ++J;
      if (Base && J == Held && P->Tail++ != 2 * MaxScope)
        Scope[P->Tail - 1] = Base;
    }
  }
  for (int32_t I = MaxScope;
       !Name && !Address && I < P->Tail && I != 2 * MaxScope; ++I)
    if ((Address = bindIn(P, Scope[I], 0, Own, Now)) && !P->Check &&
        LdrAddRefDll(1, Scope[I]) < 0)
      P->Status = STATUS_DLL_INIT_FAILED;
  for (const char *M = *(const char *const *)Head;
       !Address && (Name || P->Tail > 2 * MaxScope) && M != Head;
       M = *(const char *const *)M) {
    const char *Base = markedImage(M, !Name);
    uint32_t J = 0;
    while (J != Held && Scope[J] != Base)
      ++J;
    if (Base && J == Held && (Address = bindIn(P, Base, Name, Own, Now)) &&
        !Name && !P->Check && LdrAddRefDll(1, Base) < 0)
      P->Status = STATUS_DLL_INIT_FAILED;
  }
  return Address || Name || !(*(const uint16_t *)(Own + 20) & OfferAddition)
             ? Address
             : foreign(P, Own);
}

// The address that a weak import's DLL, named at Dll, exports as Name, or
// zero where the process has not loaded the DLL, which this does not load
// or pin.
static __attribute__((noinline)) uint64_t weakImport(const uint16_t *Dll,
                                                     const char *Name) {
  uint16_t Length = 0;
  while (Dll[Length])
    ++Length;
  struct UnicodeString String = {(uint16_t)(2 * Length),
                                 (uint16_t)(2 * Length + 2), Dll};
  void *Module;
  return LdrGetDllHandleEx(1, 0, 0, &String, &Module) < 0
             ? 0
             : exportOf(Module, imageSize(Module), "", Name);
}

enum {
  TableMagic = 0x7B3AC2D5,
  TableFormat = 1,
  TableTag = 0xC5E1A7F3,
  AnchorHeader = 0x18
};

// The executable's override table, which its anchor confirms: what each slot
// names, the slots that a definition replaces, and for each slot the one
// whose target a call of its default reaches. No slot is tagged where the
// executable has no table. And the executable's offer table, which has no
// entries where it has none that fits it.
struct Table {
  uint32_t Replaced, Tagged;
  uint64_t Target[32];
  uint8_t Call[32];
  struct Offers Offers;
};

// Reads the table of the executable that the kernel maps, and returns zero,
// or a negative status where the table and its anchor disagree, either has
// been changed, the anchor is in the executable and the table is not, or,
// unless Check, a slot names what control flow guard would not let a call
// reach: a word that the fill seals holds no address that a check would not
// accept.
static __attribute__((noinline)) int32_t readTable(struct Table *T, int Check) {
  T->Replaced = T->Tagged = 0;
  T->Offers = offersOf(0, 0, 0);
  const char *Base = executable();
  if (!Base)
    return 0;
  uint32_t Size = imageSize(Base);
  T->Offers = offersOf(Base, Size, offerTable(Base, Size));
  // The anchor is the section .ovra, which is read-only.
  const char *Nt = Base + *(const uint32_t *)(Base + 0x3c);
  const char *Sec = Nt + 24 + *(const uint16_t *)(Nt + 20), *Anchor = 0;
  uint32_t AnchorSize = 0;
  for (uint32_t N = *(const uint16_t *)(Nt + 6); N && !Anchor; --N, Sec += 40)
    if (*(const uint64_t *)Sec == 0x6172766f2e && !(Sec[39] & 0x80)) {
      Anchor = Base + *(const uint32_t *)(Sec + 12);
      AnchorSize = *(const uint32_t *)(Sec + 8);
    }
  const char *Header = Base + 0x80;
  uint32_t Count = *(const uint16_t *)(Header + 6);
  if (*(const uint32_t *)Header != TableMagic)
    return Anchor && AnchorSize >= 4 && *(const uint32_t *)Anchor == TableMagic
               ? STATUS_INVALID_IMAGE_HASH
               : 0;
  // The anchor's header begins as the table's does.
  if (!Anchor || *(const uint16_t *)(Header + 4) != TableFormat || Count > 32 ||
      AnchorSize < AnchorHeader + Count * 9 ||
      Base + *(const uint32_t *)(Header + 8) != Anchor ||
      *(const uint64_t *)Anchor != *(const uint64_t *)Header ||
      *(const uint32_t *)(Anchor + 12) != *(const uint32_t *)(Base + 0x38))
    return STATUS_INVALID_IMAGE_HASH;
  uint32_t Replaced = *(const uint32_t *)(Anchor + 8);
  uint32_t Offers = *(const uint32_t *)(Anchor + 20), Tagged = 0;
  for (uint32_t K = 0; K != Count; ++K) {
    // The first eight slots precede the header, and the others follow it.
    const uint32_t *Slot =
        (const uint32_t *)(Base + 8 * K + (K < 8 ? 0x40 : 0x58));
    uint64_t Entry = *(const Unaligned *)(Anchor + AnchorHeader + 8 * K);
    if (!Slot[0] && !Slot[1] && !Entry)
      continue;
    if (Slot[0] != (TableTag ^ K) || Slot[1] >= Size ||
        (uint64_t)Base + Slot[1] != Entry)
      return STATUS_INVALID_IMAGE_HASH;
    Tagged |= 1u << K;
    // An offer is the DLL's function, to which the executable's import of
    // it is bound.
    uint64_t To = T->Target[K] =
        Offers >> K & 1 ? *(const Unaligned *)Entry : Entry;
    const char *In = Offers >> K & 1 ? imageAt((const void *)To) : Base;
    if (!Check && (!In || guardOf(In, imageSize(In), To - (uint64_t)In) < 0))
      return STATUS_INVALID_IMAGE_HASH;
  }
  for (uint32_t K = 0; K != Count; ++K) {
    uint8_t C = (uint8_t)Anchor[AnchorHeader + 8 * Count + K];
    if ((Tagged >> K & 1) &&
        (C >= Count || !(Tagged >> C & 1) || (C != K && !(Replaced >> C & 1))))
      return STATUS_INVALID_IMAGE_HASH;
    T->Call[K] = C;
  }
  if ((Replaced | Offers) & ~Tagged)
    return STATUS_INVALID_IMAGE_HASH;
  T->Replaced = Replaced;
  T->Tagged = Tagged;
  return 0;
}

// Whether the executable's link saw this DLL as one that the process loads as
// it starts, so that its offer table names each instance that the DLL shares
// with another image that the link saw: the table has the entry of the DLL's
// own table that names the DLL.
static int seen(struct Pass *P, const struct Table *T,
                const struct FillList *L) {
  return L->Self && offerOf(P, &T->Offers, __ImageBase + L->Self, 0, 0);
}
// Whether the DLL searches: the process loaded it after it started, which
// Loaded says, or the executable's link did not see it. Found once, where a
// walk first needs to know.
static inline __attribute__((always_inline)) int
searches(struct Pass *P, const struct Table *T, const struct FillList *L) {
  return P->Loaded < 0 ? P->Loaded = !seen(P, T, L) : P->Loaded;
}

// Writes the name words W, which follow the vague cells: each takes the name
// word of the instance that the executable's table T or the search names
// where the two name one blob, and otherwise holds the image's own. Their
// search starts from the image's own instance, which needs no name compared
// where it is the one found, and reads only the images that the process
// loaded as it started: it neither pins nor fails, and two images that the
// process loads later compare their names. A DLL that does not search passes
// over them where T has no type_info object. A first fill binds a word once,
// and binds none while its round holds as many changes as it can.
static __attribute__((noinline)) void
bindNames(struct Pass *P, const struct Table *T, const struct FillWord *W) {
  if (P->First && !hasDescriptors(&T->Offers) && !searches(P, T, P->List))
    return;
  for (const struct FillWord *End = W + P->List->Names; W != End && !P->Full;
       ++W) {
    // A bound word names another blob of the same length.
    uint32_t Blob = (uint64_t)W->Addend >> 32;
    uint64_t Word = *(const Unaligned *)(__ImageBase + W->Word);
    if (P->First && !P->Check && !P->Collided &&
        (Word ^ ((uint64_t)__ImageBase + Blob)) << 16)
      continue;
    const char *Own = __ImageBase + (uint32_t)W->Addend;
    uint64_t Now = (uint64_t)__ImageBase + *(const uint32_t *)(Own + 12);
    uint64_t Mine = nameWord(Own, Blob);
    const char *E = offerEntry(&T->Offers, W->Source >> 8, 1);
    uint64_t Value = E ? offerOf(P, &T->Offers, Own, Now, E) : 0;
    if (!Value && searches(P, T, P->List))
      for (uint32_t I = 1, Held = scopeOf(P);
           !Value && I < Held && I != MaxScope; ++I)
        Value = bindIn(P, P->Scope[I], 0, Own, Now);
    if (Value)
      Value = nameOf(Value, Mine, Own);
    if (Value || !P->First || P->Check)
      put(P, W->Word, Value ? Value : Mine);
  }
}

// Computes every word: the cells first, from the executable's table, which
// is read and checked before any word, and then the imports, which other
// words are computed from. A missing import fails the image unless it is
// weak. Only an image that binds an import by name, Imports, searches.
static inline __attribute__((always_inline)) void walkWords(struct Pass *P,
                                                            int Imports) {
  const struct FillList *L = P->List;
  const struct FillWord *W =
      (const struct FillWord *)((const char *)(L + 1) + 8 * L->Ranges);
  struct Table T __attribute__((uninitialized));
  if (L->Cells && (P->Status = readTable(&T, P->Check)) < 0)
    return;
  // The vague cells, which come first and only a DLL that searches has, each
  // take the instance that the executable's table names, which the top bits
  // of the key in its word rule out without the cell's own entry where it
  // names none. A DLL that the process loaded after it started, or that the
  // executable's link did not see, also searches the other images. An image's
  // own word otherwise holds its own instance, which a first fill finds it
  // holding, and an import keeps what the loader bound it to.
  uint32_t I = 0;
  for (; Imports && I != L->Cells &&
         (W[I].Source & 63) == (WordCell | CellVague << 3);
       ++I) {
    const char *Own = __ImageBase + (uint32_t)W[I].Addend;
    const Unaligned *Now = (const Unaligned *)(__ImageBase + W[I].Word);
    const char *E = offerEntry(&T.Offers, W[I].Source >> 8, 1);
    uint64_t Value = E ? offerOf(P, &T.Offers, Own, *Now, E) : 0;
    if (!Value && searches(P, &T, L)) {
      Value = search(P, 0, Own, *Now);
      if (P->Status < 0)
        return;
    }
    if (!Value) {
      uint32_t Instance = (uint64_t)W[I].Addend >> 32;
      if (!Instance || (P->First && !P->Check))
        continue;
      Value = (uint64_t)__ImageBase + Instance;
    }
    put(P, W[I].Word, Value);
  }
  if (Imports && L->Names)
    bindNames(P, &T, W + I);
  I += L->Names;
  for (; I != L->Words; ++I) {
    uint64_t Value;
    if ((W[I].Source & 7) == WordCell) {
      // A slot that the table names nothing for leaves the word as the loader
      // bound it, or gives the image's own word its default.
      uint32_t K = W[I].Source >> 8 & 31, Use = W[I].Source >> 3 & 7;
      if ((Use == CellAddress ? T.Replaced : T.Tagged) >> K & 1)
        Value = T.Target[Use == CellCall ? T.Call[K] : K];
      else if (W[I].Addend)
        Value = (uint64_t)__ImageBase + W[I].Addend;
      else
        continue;
    } else if (Imports && (W[I].Source & 7) == WordImport) {
      const char *Name = __ImageBase + (uint32_t)W[I].Addend;
      uint32_t Dll = (uint64_t)W[I].Addend >> 32;
      Value = Dll > 1 ? weakImport((const uint16_t *)(__ImageBase + Dll), Name)
                      : search(P, Name, 0, 0);
      if (!Value && !Dll) {
        P->Status = STATUS_ENTRYPOINT_NOT_FOUND;
        return;
      }
    } else {
      Value = valueAt(P, W[I].Source & ~7u);
      switch (W[I].Source & 7) {
      case WordValue:
        Value = *(const uint64_t *)Value;
        break;
      case WordInit:
        // A function runs once every other word holds its value: the first
        // round leaves it to the next.
        if (!P->Inits) {
          P->Full = 1;
          continue;
        }
        Value = ((uint64_t (*)(void *))Value)(__ImageBase);
        if (!Value) {
          P->Status = STATUS_DLL_INIT_FAILED;
          return;
        }
        break;
      case WordTlsOffset: {
        // The record's image's place, which this pass may have computed where
        // the image is this one, plus the record's word at Addend. An image
        // of a scale other than 1 is reached through its record.
        if (!P->Inits) {
          P->Full = 1;
          continue;
        }
        const char *Record = (const char *)Value;
        uint64_t Place = *(const uint64_t *)Record,
                 At = Place - (uint64_t)__ImageBase;
        Value = At >> 32 ? *(const uint64_t *)Place : valueAt(P, (uint32_t)At);
        if (!Value || *(const uint64_t *)(Place + 8) != 1) {
          P->Status = STATUS_DLL_INIT_FAILED;
          return;
        }
        put(P, W[I].Word, Value + *(const uint32_t *)(Record + W[I].Addend));
        continue;
      }
      }
      Value += (uint64_t)W[I].Addend;
    }
    put(P, W[I].Word, Value);
  }
}

// The walks of an image that binds no import by name and of one that does,
// which alone holds the search.
static __attribute__((noinline)) void walkLocal(struct Pass *P) {
  walkWords(P, 0);
}
static __attribute__((noinline)) void walkImports(struct Pass *P) {
  walkWords(P, 1);
}

// Computes every word that the list names, from read-only memory alone.
static inline __attribute__((always_inline)) void walk(struct Pass *P,
                                                       int Imports) {
  if (Imports)
    walkImports(P);
  else
    walkLocal(P);
}

enum {
  ProcessList = 5,
  ThreadBasic = 0,
  ThreadImpersonation = 5,
  TokenImpersonate = 4,
  ThreadSuspendGetContext = 0xa | 0x800,
  MemCommitReserve = 0x3000,
  MemRelease = 0x8000,
};
#define STATUS_INFO_LENGTH_MISMATCH ((int32_t)0xC0000004)
#define STATUS_INVALID_CID ((int32_t)0xC000000B)
#define STATUS_THREAD_IS_TERMINATING ((int32_t)0xC000004B)
#define STATUS_NO_TOKEN ((int32_t)0xC000007C)
#define STATUS_ACCESS_DENIED ((int32_t)0xC0000022)

// Writes the thread-local words of the thread that Thread names, suspended
// once reading its context has made the suspension take effect, so that it
// neither runs nor frees its copies meanwhile; one that ends meanwhile runs
// no more code. A thread that this suspends and cannot resume would hang the
// process, which ends instead.
static int32_t fillThread(const struct FillList *L, void *Thread) {
  struct {
    int32_t ExitStatus;
    const char *Teb;
    struct ClientId Id;
    uintptr_t Affinity;
    int32_t Priority, BasePriority;
  } Basic;
  // A CONTEXT, of which the kernel reads only the flags, at 0x30 on x86-64,
  // so that no memset is called, which an NT-POSIX image does not link.
#if defined(__x86_64__)
  _Alignas(16) uint32_t Context[0x4d0 / 4] __attribute__((uninitialized));
  Context[12] = 0x100001;
#else
  _Alignas(16) uint32_t Context[0x390 / 4] __attribute__((uninitialized));
  Context[0] = 0x400001;
#endif
  int32_t Status =
      NtQueryInformationThread(Thread, ThreadBasic, &Basic, sizeof(Basic), 0);
  if (Status < 0 || Basic.Id.Thread == *(const uintptr_t *)(teb() + 0x48))
    return Status;
  if ((Status = NtSuspendThread(Thread, 0)) >= 0) {
    if ((Status = NtGetContextThread(Thread, Context)) >= 0)
      fillCopy(L, Basic.Teb);
    int32_t Resumed = NtResumeThread(Thread, 0);
    if (Resumed < 0)
      NtTerminateProcess((void *)-1, Resumed);
  }
  return Status == STATUS_THREAD_IS_TERMINATING ? 0 : Status;
}

// Writes the thread-local words of each thread of the process that the
// system lists, once the template is written, other than the running one,
// and returns a negative status where one cannot be reached. The list names
// every thread of a process that the caller may query or whose integrity is
// at least medium, which then opens each by its thread and this process's id,
// which the kernel refuses for a thread of another process; a thread that
// starts later copies the written template. Otherwise this opens the threads
// it may open.
static int32_t fillListed(const struct FillList *L) {
  // Asking for the size would list every process twice; the list grows with
  // every thread that starts meanwhile.
  uint32_t Need = 1 << 20;
  int32_t Status;
  char *List;
  uintptr_t Size;
  do {
    List = 0;
    Size = Need + Need / 4;
    if ((Status = NtAllocateVirtualMemory((void *)-1, (void **)&List, 0, &Size,
                                          MemCommitReserve, PageReadWrite)) < 0)
      return Status;
    Status = NtQuerySystemInformation(ProcessList, List, (uint32_t)Size, &Need);
    if (Status < 0) {
      Size = 0;
      NtFreeVirtualMemory((void *)-1, (void **)&List, &Size, MemRelease);
    }
  } while (Status == STATUS_INFO_LENGTH_MISMATCH);
  if (Status < 0)
    return Status;
  // Each process's entry, of 0x100 bytes with its id at 0x50, is followed by
  // its threads', of 0x50 bytes with their ids at 0x28.
  const char *Teb = teb(), *Entry = List;
  struct ClientId Id = {*(const uintptr_t *)(Teb + 0x40), 0};
  while (*(const uintptr_t *)(Entry + 0x50) != Id.Process &&
         *(const uint32_t *)Entry)
    Entry += *(const uint32_t *)Entry;
  int Listed = *(const uintptr_t *)(Entry + 0x50) == Id.Process;
  const char *T = Entry + 0x100;
  for (uint32_t N = Listed ? *(const uint32_t *)(Entry + 4) : 0;
       N && Status >= 0; --N, T += 0x50) {
    struct ObjectAttributes Attributes = {sizeof(Attributes), 0, 0, 0, 0, 0};
    void *Thread;
    Id.Thread = *(const uintptr_t *)(T + 0x30);
    // A thread that has ended since is not there to open.
    if ((Status = NtOpenThread(&Thread, ThreadSuspendGetContext, &Attributes,
                               &Id)) == STATUS_INVALID_CID)
      Status = 0;
    else if (Status >= 0) {
      Status = fillThread(L, Thread);
      NtClose(Thread);
    }
  }
  Size = 0;
  NtFreeVirtualMemory((void *)-1, (void **)&List, &Size, MemRelease);
  if (Listed)
    return Status;
  void *Thread = 0, *Next;
  while (Status >= 0 &&
         NtGetNextThread((void *)-1, Thread, ThreadSuspendGetContext, 0, 0,
                         &Next) >= 0) {
    if (Thread)
      NtClose(Thread);
    Status = fillThread(L, Thread = Next);
  }
  if (Thread)
    NtClose(Thread);
  return Status;
}

// Writes the thread-local words of every other thread of the process, which
// a DLL fails its load with where that is negative. A thread that
// impersonates opens them as the process does meanwhile: it fails where it
// still holds a token once it has given its own up, and the process ends
// where the thread does not hold one again afterwards, which would otherwise
// go on with the process's rights.
static __attribute__((noinline)) int32_t fillThreads(const struct FillList *L) {
  void *Token = 0, *None = 0, *Held;
  int32_t Status = 0;
  if (NtOpenThreadToken((void *)-2, TokenImpersonate, 1, &Token) >= 0 &&
      (Status = NtSetInformationThread((void *)-2, ThreadImpersonation, &None,
                                       sizeof(None))) >= 0) {
    Status = NtOpenThreadToken((void *)-2, TokenImpersonate, 1, &Held);
    if (Status >= 0)
      NtClose(Held);
    Status = Status == STATUS_NO_TOKEN ? 0 : STATUS_ACCESS_DENIED;
  }
  if (Status >= 0)
    Status = fillListed(L);
  if (Token) {
    int32_t Restored = NtSetInformationThread((void *)-2, ThreadImpersonation,
                                              &Token, sizeof(Token));
    if (Restored >= 0 && (Restored = NtOpenThreadToken(
                              (void *)-2, TokenImpersonate, 1, &Held)) >= 0)
      NtClose(Held);
    if (Restored < 0)
      NtTerminateProcess((void *)-1, Restored);
    NtClose(Token);
  }
  return Status;
}

// Writes the image's words, and returns a status that is negative where the
// image cannot run. Again makes it write an image that it has written, whose
// words another program's start may have left stale. Read-only memory is
// writable only around the stores of the words that changed, as many at a
// time as a pass collects, and what it then holds is checked against a second
// computation, so that a write that reached it meanwhile fails the image.
// Loaded says that a DLL was loaded after the process started, and is
// negative where that is not known; Visit that the fill writes the
// thread-local words of every other thread, and otherwise only the running
// thread's.
static inline __attribute__((always_inline)) int32_t fill(uint32_t Again,
                                                          int Imports,
                                                          int Loaded,
                                                          int Visit) {
  const struct FillList *L = &__llvm_fill_list;
  Unaligned *Done = (Unaligned *)(__ImageBase + L->Sealed.Rva);
  // An image in which the fill writes no read-only memory has no such word.
  if (L->Sealed.Size && *Done && !Again)
    return 0;
  struct Pass P __attribute__((uninitialized));
  const char *Scope[2 * MaxScope] __attribute__((uninitialized));
  P.List = L;
  P.Check = 0;
  P.Loaded = Loaded;
  P.Status = P.Collided = 0;
  P.First = !Again;
  P.Count = P.Tail = -1;
  P.Scope = Imports ? Scope : 0;
  int Written = P.Inits = 0;
  do {
    P.Changes = P.Full = 0;
    walk(&P, Imports);
    if (P.Status < 0)
      return P.Status;
    // Where every read-only word holds its value, nothing is opened.
    if (P.Changes) {
      int32_t Opened = protect(L, PageReadWrite);
      if (Opened >= 0) {
        for (uint32_t I = 0; I != P.Changes; ++I)
          *(Unaligned *)(__ImageBase + P.Rva[I]) = P.Value[I];
        *Done = 1;
      }
      int32_t Sealed = protect(L, PageReadOnly);
      if (Opened < 0 || Sealed < 0)
        return Opened < 0 ? Opened : Sealed;
      Written = 1;
    }
    // A round that left a read-only word for the next runs no function.
    P.Inits = P.Changes != MaxChanges;
  } while (P.Full);
  if (Written) {
    P.Check = 1;
    walk(&P, Imports);
  }
  if (P.Status >= 0 && L->ThreadLocalWords) {
    fillCopy(L, teb());
    if (Visit)
      P.Status = fillThreads(L);
  }
  return P.Status;
}

// Each entry passes the reserved argument of a DLL's attach, which the loader
// gives only a DLL that the process loads as it starts, and an executable's
// whatever it has.
int32_t NTAPI __llvm_fill(uint32_t Again, const void *Reserved) {
  (void)Reserved;
  return fill(Again, 0, 0, 0);
}

// The fill of an image that binds imports by name or is a DLL with vague
// cells, which the image's entry calls in place of __llvm_fill, so that no
// other image holds the search or imports what it calls.
int32_t NTAPI __llvm_fill_imports(uint32_t Again, const void *Reserved) {
  return fill(Again, 1, Reserved ? -1 : 1, 0);
}

// The fills of a DLL whose template holds a thread-local word, which also
// write it in every other thread where the process loaded the DLL after it
// started, so that no other image holds what that calls.
int32_t NTAPI __llvm_fill_threads(uint32_t Again, const void *Reserved) {
  return fill(Again, 0, 0, !Reserved);
}
int32_t NTAPI __llvm_fill_imports_threads(uint32_t Again,
                                          const void *Reserved) {
  return fill(Again, 1, Reserved ? -1 : 1, !Reserved);
}

// The first TLS callback of a DLL whose template holds a thread-local word,
// which writes it in each thread that attaches: one the loader started while
// it loaded the DLL copied the template before the fill wrote it.
static void NTAPI attach(void *Module, uint32_t Reason, void *Reserved) {
  (void)Module, (void)Reserved;
  if (Reason == 2)
    fillCopy(&__llvm_fill_list, teb());
}

// The first TLS callback of an executable whose template holds a thread-local
// word, which runs the fill in place of the entry point: the loader starts
// every other thread once these callbacks have run, which then copy the
// written template, but runs the executable's entry point only after that.
// A fill that fails ends the process, as the entry would.
static inline __attribute__((always_inline)) void
exeAttach(void *Module, uint32_t Reason, void *Reserved, int Imports) {
  (void)Module, (void)Reserved;
  int32_t Status = Reason == 1 ? fill(0, Imports, 0, 0) : 0;
  if (Status < 0)
    RtlExitUserProcess(Status);
}
static void NTAPI exeLocal(void *Module, uint32_t Reason, void *Reserved) {
  exeAttach(Module, Reason, Reserved, 0);
}
static void NTAPI exeImports(void *Module, uint32_t Reason, void *Reserved) {
  exeAttach(Module, Reason, Reserved, 1);
}

// Each in .CRT$XLA$fill, which sorts after the start of the callbacks and
// before any other, where the linker keeps the one the image needs.
typedef void(NTAPI *TlsCallback)(void *, uint32_t, void *);
__attribute__((selectany, section(".CRT$XLA$fill")))
const TlsCallback __llvm_fill_attach = attach,
                  __llvm_fill_exe_attach = exeLocal,
                  __llvm_fill_exe_attach_imports = exeImports;

#pragma clang attribute pop
