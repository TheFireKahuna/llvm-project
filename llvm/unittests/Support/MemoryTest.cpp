//===- llvm/unittest/Support/AllocatorTest.cpp - BumpPtrAllocator tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Memory.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "gtest/gtest.h"
#include <cstdlib>
#include <cstring>

#ifdef LLVM_RUNTIME_WIN32
#include <windows.h>
#endif

#if defined(__NetBSD__)
// clang-format off
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <err.h>
#include <unistd.h>
// clang-format on
#endif

using namespace llvm;
using namespace sys;

extern const char *TestMainArgv0;

namespace {

bool IsMPROTECT() {
#if defined(__NetBSD__)
  int mib[3];
  int paxflags;
  size_t len = sizeof(paxflags);

  mib[0] = CTL_PROC;
  mib[1] = getpid();
  mib[2] = PROC_PID_PAXFLAGS;

  if (sysctl(mib, 3, &paxflags, &len, NULL, 0) != 0)
    err(EXIT_FAILURE, "sysctl");

  return !!(paxflags & CTL_PROC_PAXFLAGS_MPROTECT);
#elif (defined(__APPLE__) && defined(__aarch64__)) || defined(__OpenBSD__)
  return true;
#else
  return false;
#endif
}

class MappedMemoryTest : public ::testing::TestWithParam<unsigned> {
public:
  MappedMemoryTest() {
    Flags = GetParam();
    PageSize = sys::Process::getPageSizeEstimate();
  }

protected:
  // Adds RW flags to permit testing of the resulting memory
  unsigned getTestableEquivalent(unsigned RequestedFlags) {
    switch (RequestedFlags) {
    case Memory::MF_READ:
    case Memory::MF_WRITE:
    case Memory::MF_READ|Memory::MF_WRITE:
      return Memory::MF_READ|Memory::MF_WRITE;
    case Memory::MF_READ|Memory::MF_EXEC:
    case Memory::MF_READ|Memory::MF_WRITE|Memory::MF_EXEC:
    case Memory::MF_EXEC:
      return Memory::MF_READ|Memory::MF_WRITE|Memory::MF_EXEC;
    }
    // Default in case values are added to the enum, as required by some compilers
    return Memory::MF_READ|Memory::MF_WRITE;
  }

  // Returns true if the memory blocks overlap
  bool doesOverlap(MemoryBlock M1, MemoryBlock M2) {
    if (M1.base() == M2.base())
      return true;

    if (M1.base() > M2.base())
      return (unsigned char *)M2.base() + M2.allocatedSize() > M1.base();

    return (unsigned char *)M1.base() + M1.allocatedSize() > M2.base();
  }

  unsigned Flags;
  size_t   PageSize;
};

// MPROTECT prevents W+X mmaps
#define CHECK_UNSUPPORTED() \
  do { \
    if ((Flags & Memory::MF_WRITE) && (Flags & Memory::MF_EXEC) && \
        IsMPROTECT()) \
      GTEST_SKIP();   \
  } while (0)

TEST_P(MappedMemoryTest, AllocAndRelease) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock M1 = Memory::allocateMappedMemory(sizeof(int), nullptr, Flags,EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(sizeof(int), M1.allocatedSize());

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
}

TEST_P(MappedMemoryTest, AllocAndReleaseHuge) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock M1 = Memory::allocateMappedMemory(
      sizeof(int), nullptr, Flags | Memory::MF_HUGE_HINT, EC);
  EXPECT_EQ(std::error_code(), EC);

  // Test large/huge memory pages. In the worst case, 4kb pages should be
  // returned, if large pages aren't available.

  EXPECT_NE((void *)nullptr, M1.base());
  EXPECT_LE(sizeof(int), M1.allocatedSize());

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
}

TEST_P(MappedMemoryTest, MultipleAllocAndRelease) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock M1 = Memory::allocateMappedMemory(16, nullptr, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M2 = Memory::allocateMappedMemory(64, nullptr, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M3 = Memory::allocateMappedMemory(32, nullptr, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(16U, M1.allocatedSize());
  EXPECT_NE((void*)nullptr, M2.base());
  EXPECT_LE(64U, M2.allocatedSize());
  EXPECT_NE((void*)nullptr, M3.base());
  EXPECT_LE(32U, M3.allocatedSize());

  EXPECT_FALSE(doesOverlap(M1, M2));
  EXPECT_FALSE(doesOverlap(M2, M3));
  EXPECT_FALSE(doesOverlap(M1, M3));

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
  EXPECT_FALSE(Memory::releaseMappedMemory(M3));
  MemoryBlock M4 = Memory::allocateMappedMemory(16, nullptr, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  EXPECT_NE((void*)nullptr, M4.base());
  EXPECT_LE(16U, M4.allocatedSize());
  EXPECT_FALSE(Memory::releaseMappedMemory(M4));
  EXPECT_FALSE(Memory::releaseMappedMemory(M2));
}

TEST_P(MappedMemoryTest, BasicWrite) {
  // This test applies only to readable and writeable combinations
  if (Flags &&
      !((Flags & Memory::MF_READ) && (Flags & Memory::MF_WRITE)))
    GTEST_SKIP();
  CHECK_UNSUPPORTED();

  std::error_code EC;
  MemoryBlock M1 = Memory::allocateMappedMemory(sizeof(int), nullptr, Flags,EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(sizeof(int), M1.allocatedSize());

  int *a = (int*)M1.base();
  *a = 1;
  EXPECT_EQ(1, *a);

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
}

TEST_P(MappedMemoryTest, MultipleWrite) {
  // This test applies only to readable and writeable combinations
  if (Flags &&
      !((Flags & Memory::MF_READ) && (Flags & Memory::MF_WRITE)))
    GTEST_SKIP();
  CHECK_UNSUPPORTED();

  std::error_code EC;
  MemoryBlock M1 = Memory::allocateMappedMemory(sizeof(int), nullptr, Flags,
                                                EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M2 = Memory::allocateMappedMemory(8 * sizeof(int), nullptr, Flags,
                                                EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M3 = Memory::allocateMappedMemory(4 * sizeof(int), nullptr, Flags,
                                                EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_FALSE(doesOverlap(M1, M2));
  EXPECT_FALSE(doesOverlap(M2, M3));
  EXPECT_FALSE(doesOverlap(M1, M3));

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(1U * sizeof(int), M1.allocatedSize());
  EXPECT_NE((void*)nullptr, M2.base());
  EXPECT_LE(8U * sizeof(int), M2.allocatedSize());
  EXPECT_NE((void*)nullptr, M3.base());
  EXPECT_LE(4U * sizeof(int), M3.allocatedSize());

  int *x = (int*)M1.base();
  *x = 1;

  int *y = (int*)M2.base();
  for (int i = 0; i < 8; i++) {
    y[i] = i;
  }

  int *z = (int*)M3.base();
  *z = 42;

  EXPECT_EQ(1, *x);
  EXPECT_EQ(7, y[7]);
  EXPECT_EQ(42, *z);

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
  EXPECT_FALSE(Memory::releaseMappedMemory(M3));

  MemoryBlock M4 = Memory::allocateMappedMemory(64 * sizeof(int), nullptr,
                                                Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  EXPECT_NE((void*)nullptr, M4.base());
  EXPECT_LE(64U * sizeof(int), M4.allocatedSize());
  x = (int*)M4.base();
  *x = 4;
  EXPECT_EQ(4, *x);
  EXPECT_FALSE(Memory::releaseMappedMemory(M4));

  // Verify that M2 remains unaffected by other activity
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(i, y[i]);
  }
  EXPECT_FALSE(Memory::releaseMappedMemory(M2));
}

TEST_P(MappedMemoryTest, EnabledWrite) {
  // MPROTECT prevents W+X, and since this test always adds W we need
  // to block any variant with X.
  if ((Flags & Memory::MF_EXEC) && IsMPROTECT())
    GTEST_SKIP();

  std::error_code EC;
  MemoryBlock M1 = Memory::allocateMappedMemory(2 * sizeof(int), nullptr, Flags,
                                                EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M2 = Memory::allocateMappedMemory(8 * sizeof(int), nullptr, Flags,
                                                EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M3 = Memory::allocateMappedMemory(4 * sizeof(int), nullptr, Flags,
                                                EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(2U * sizeof(int), M1.allocatedSize());
  EXPECT_NE((void*)nullptr, M2.base());
  EXPECT_LE(8U * sizeof(int), M2.allocatedSize());
  EXPECT_NE((void*)nullptr, M3.base());
  EXPECT_LE(4U * sizeof(int), M3.allocatedSize());

  EXPECT_FALSE(Memory::protectMappedMemory(M1, getTestableEquivalent(Flags)));
  EXPECT_FALSE(Memory::protectMappedMemory(M2, getTestableEquivalent(Flags)));
  EXPECT_FALSE(Memory::protectMappedMemory(M3, getTestableEquivalent(Flags)));

  EXPECT_FALSE(doesOverlap(M1, M2));
  EXPECT_FALSE(doesOverlap(M2, M3));
  EXPECT_FALSE(doesOverlap(M1, M3));

  int *x = (int*)M1.base();
  *x = 1;
  int *y = (int*)M2.base();
  for (unsigned int i = 0; i < 8; i++) {
    y[i] = i;
  }
  int *z = (int*)M3.base();
  *z = 42;

  EXPECT_EQ(1, *x);
  EXPECT_EQ(7, y[7]);
  EXPECT_EQ(42, *z);

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
  EXPECT_FALSE(Memory::releaseMappedMemory(M3));
  EXPECT_EQ(6, y[6]);

  MemoryBlock M4 = Memory::allocateMappedMemory(16, nullptr, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  EXPECT_NE((void*)nullptr, M4.base());
  EXPECT_LE(16U, M4.allocatedSize());
  EXPECT_EQ(std::error_code(),
            Memory::protectMappedMemory(M4, getTestableEquivalent(Flags)));
  x = (int*)M4.base();
  *x = 4;
  EXPECT_EQ(4, *x);
  EXPECT_FALSE(Memory::releaseMappedMemory(M4));
  EXPECT_FALSE(Memory::releaseMappedMemory(M2));
}

TEST_P(MappedMemoryTest, SuccessiveNear) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock M1 = Memory::allocateMappedMemory(16, nullptr, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M2 = Memory::allocateMappedMemory(64, &M1, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M3 = Memory::allocateMappedMemory(32, &M2, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(16U, M1.allocatedSize());
  EXPECT_NE((void*)nullptr, M2.base());
  EXPECT_LE(64U, M2.allocatedSize());
  EXPECT_NE((void*)nullptr, M3.base());
  EXPECT_LE(32U, M3.allocatedSize());

  EXPECT_FALSE(doesOverlap(M1, M2));
  EXPECT_FALSE(doesOverlap(M2, M3));
  EXPECT_FALSE(doesOverlap(M1, M3));

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
  EXPECT_FALSE(Memory::releaseMappedMemory(M3));
  EXPECT_FALSE(Memory::releaseMappedMemory(M2));
}

TEST_P(MappedMemoryTest, DuplicateNear) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock Near((void*)(3*PageSize), 16);
  MemoryBlock M1 = Memory::allocateMappedMemory(16, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M2 = Memory::allocateMappedMemory(64, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M3 = Memory::allocateMappedMemory(32, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(16U, M1.allocatedSize());
  EXPECT_NE((void*)nullptr, M2.base());
  EXPECT_LE(64U, M2.allocatedSize());
  EXPECT_NE((void*)nullptr, M3.base());
  EXPECT_LE(32U, M3.allocatedSize());

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
  EXPECT_FALSE(Memory::releaseMappedMemory(M3));
  EXPECT_FALSE(Memory::releaseMappedMemory(M2));
}

TEST_P(MappedMemoryTest, ZeroNear) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock Near(nullptr, 0);
  MemoryBlock M1 = Memory::allocateMappedMemory(16, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M2 = Memory::allocateMappedMemory(64, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M3 = Memory::allocateMappedMemory(32, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(16U, M1.allocatedSize());
  EXPECT_NE((void*)nullptr, M2.base());
  EXPECT_LE(64U, M2.allocatedSize());
  EXPECT_NE((void*)nullptr, M3.base());
  EXPECT_LE(32U, M3.allocatedSize());

  EXPECT_FALSE(doesOverlap(M1, M2));
  EXPECT_FALSE(doesOverlap(M2, M3));
  EXPECT_FALSE(doesOverlap(M1, M3));

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
  EXPECT_FALSE(Memory::releaseMappedMemory(M3));
  EXPECT_FALSE(Memory::releaseMappedMemory(M2));
}

TEST_P(MappedMemoryTest, ZeroSizeNear) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock Near((void*)(4*PageSize), 0);
  MemoryBlock M1 = Memory::allocateMappedMemory(16, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M2 = Memory::allocateMappedMemory(64, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);
  MemoryBlock M3 = Memory::allocateMappedMemory(32, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(16U, M1.allocatedSize());
  EXPECT_NE((void*)nullptr, M2.base());
  EXPECT_LE(64U, M2.allocatedSize());
  EXPECT_NE((void*)nullptr, M3.base());
  EXPECT_LE(32U, M3.allocatedSize());

  EXPECT_FALSE(doesOverlap(M1, M2));
  EXPECT_FALSE(doesOverlap(M2, M3));
  EXPECT_FALSE(doesOverlap(M1, M3));

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
  EXPECT_FALSE(Memory::releaseMappedMemory(M3));
  EXPECT_FALSE(Memory::releaseMappedMemory(M2));
}

TEST_P(MappedMemoryTest, UnalignedNear) {
  CHECK_UNSUPPORTED();
  std::error_code EC;
  MemoryBlock Near((void*)(2*PageSize+5), 0);
  MemoryBlock M1 = Memory::allocateMappedMemory(15, &Near, Flags, EC);
  EXPECT_EQ(std::error_code(), EC);

  EXPECT_NE((void*)nullptr, M1.base());
  EXPECT_LE(sizeof(int), M1.allocatedSize());

  EXPECT_FALSE(Memory::releaseMappedMemory(M1));
}

// Note that Memory::MF_WRITE is not supported exclusively across
// operating systems and architectures and can imply MF_READ|MF_WRITE
unsigned MemoryFlags[] = {
                           Memory::MF_READ,
                           Memory::MF_WRITE,
                           Memory::MF_READ|Memory::MF_WRITE,
                           Memory::MF_EXEC,
                           Memory::MF_READ|Memory::MF_EXEC,
                           Memory::MF_READ|Memory::MF_WRITE|Memory::MF_EXEC
                         };

INSTANTIATE_TEST_SUITE_P(AllocationTests, MappedMemoryTest,
                         ::testing::ValuesIn(MemoryFlags));

#if defined(LLVM_RUNTIME_WIN32) && (defined(_M_X64) || defined(_M_ARM64))
static int callAt(unsigned char *P) {
  volatile auto F = reinterpret_cast<int (*)()>(P);
  return F();
}

// Under Control Flow Guard, the restricted granules are valid targets only at
// the entries; one with an entry off its start, or that the block covers only
// in part, keeps its targets.
TEST(CallTargetsTest, RestrictCallTargets) {
  PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY Policy = {};
  if (!GetProcessMitigationPolicy(GetCurrentProcess(),
                                  ProcessControlFlowGuardPolicy, &Policy,
                                  sizeof(Policy)) ||
      !Policy.EnableControlFlowGuard)
    GTEST_SKIP() << "Control Flow Guard is off";
#ifdef _M_X64
  // mov eax, 42; ret
  static const unsigned char Code[] = {0xB8, 42, 0, 0, 0, 0xC3};
#else
  // mov w0, #42; ret
  static const unsigned char Code[] = {0x40, 0x05, 0x80, 0x52,
                                       0xC0, 0x03, 0x5F, 0xD6};
#endif
  std::error_code EC;
  MemoryBlock M = Memory::allocateMappedMemory(
      4096, nullptr, Memory::MF_READ | Memory::MF_WRITE, EC);
  ASSERT_FALSE(EC);
  auto *Base = static_cast<unsigned char *>(M.base());
  for (size_t Offset : {0, 16, 32, 64, 4080})
    memcpy(Base + Offset, Code, sizeof(Code));
  ASSERT_FALSE(
      Memory::protectMappedMemory(M, Memory::MF_READ | Memory::MF_EXEC));
  uintptr_t Entries[] = {reinterpret_cast<uintptr_t>(Base),
                         reinterpret_cast<uintptr_t>(Base + 32),
                         reinterpret_cast<uintptr_t>(Base + 72)};
  EXPECT_FALSE(Memory::restrictCallTargets(MemoryBlock(Base, 4088), Entries));
  EXPECT_EQ(42, callAt(Base));
  EXPECT_EQ(42, callAt(Base + 32));
  EXPECT_EQ(42, callAt(Base + 64));
  EXPECT_EQ(42, callAt(Base + 4080));
  EXPECT_DEATH(callAt(Base + 16), "");
  EXPECT_FALSE(Memory::releaseMappedMemory(M));
}
#endif

#ifdef LLVM_RUNTIME_WIN32
static int MemoryTestAnchor;

// Only a request for large pages looks up the privilege they need, which
// loads advapi32.dll. Check in a child process, which has done nothing else.
TEST(MappedMemoryTest, LargePagePrivilegeOnlyOnRequest) {
  if (getenv("LLVM_MEMORY_TEST_CHILD")) {
    // A host that imports advapi32.dll cannot tell. Success is 3, which a
    // filter that selects no test cannot return.
    if (::GetModuleHandleW(L"advapi32.dll"))
      exit(2);
    std::error_code EC;
    unsigned Flags = Memory::MF_READ | Memory::MF_WRITE;
    MemoryBlock M = Memory::allocateMappedMemory(16, nullptr, Flags, EC);
    if (EC || ::GetModuleHandleW(L"advapi32.dll"))
      exit(1);
    M = Memory::allocateMappedMemory(16, nullptr, Flags | Memory::MF_HUGE_HINT,
                                     EC);
    exit(EC || !::GetModuleHandleW(L"advapi32.dll") ? 1 : 3);
  }

  std::string Exe = fs::getMainExecutable(TestMainArgv0, &MemoryTestAnchor);
  StringRef Args[] = {
      Exe, "--gtest_filter=MappedMemoryTest.LargePagePrivilegeOnlyOnRequest"};
  ASSERT_TRUE(::SetEnvironmentVariableW(L"LLVM_MEMORY_TEST_CHILD", L"1"));
  // The child runs its one test only outside a lit shard.
  ::SetEnvironmentVariableW(L"GTEST_SHARD_INDEX", nullptr);
  ::SetEnvironmentVariableW(L"GTEST_TOTAL_SHARDS", nullptr);
  int RC = ExecuteAndWait(Exe, Args);
  ::SetEnvironmentVariableW(L"LLVM_MEMORY_TEST_CHILD", nullptr);
  if (RC == 2)
    GTEST_SKIP() << "advapi32.dll is loaded at start";
  EXPECT_EQ(3, RC);
}
#endif

}  // anonymous namespace
