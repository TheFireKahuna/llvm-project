//===- Windows/Threading.inc - Win32 Threading Implementation - -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the Win32 specific implementation of Threading functions.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_POSIX
// LLVM libc provides POSIX pthreads and sched_*affinity on Windows.
// Thread affinity uses sched_getaffinity/sched_setaffinity (GNU extension).
// EcoQoS power throttling and physical core topology require ntdll directly
// — these have no POSIX or GNU equivalents.
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Windows/WindowsSupport.h"
#include "llvm/Support/thread.h"

#include <bitset>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

// --- NT declarations for features beyond POSIX scope ---
// EcoQoS power throttling and physical core topology have no POSIX
// equivalents. Thread affinity uses sched_getaffinity/sched_setaffinity
// (GNU extension, provided by our libc). Struct types come from
// <sys/ntabi.h> via WindowsSupport.h.
extern "C" {
__declspec(dllimport) NTSTATUS NTAPI NtSetInformationThread(
    HANDLE, ULONG, void *, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtQuerySystemInformation(
    ULONG, void *, ULONG, ULONG *);
// NUMA policy — Linux extension, provided by our libc on Windows.
long get_mempolicy(int *, unsigned long *, unsigned long, void *, unsigned long);
} // extern "C"

static const auto NtCurrentThread_ = reinterpret_cast<HANDLE>(
    static_cast<LONG_PTR>(-2));

// Information classes for NT calls with no POSIX equivalent.
static constexpr ULONG ThreadPowerThrottling_ = 49;
static constexpr ULONG SysLogicalProcInfoEx_ = 107;

// EcoQoS power throttling (Win11+, no POSIX equivalent).
struct ThreadPowerThrottlingState_ {
  ULONG Version;
  ULONG ControlMask;
  ULONG StateMask;
};
static constexpr ULONG THROTTLE_VERSION_ = 1;
static constexpr ULONG THROTTLE_EXEC_SPEED_ = 1;

// --- Thread lifecycle (POSIX pthreads) ---

namespace llvm {
pthread_t
llvm_execute_on_thread_impl(void *(*ThreadFunc)(void *), void *Arg,
                            std::optional<unsigned> StackSizeInBytes) {
  int errnum;
  pthread_attr_t Attr;
  if ((errnum = ::pthread_attr_init(&Attr)) != 0)
    report_fatal_error("pthread_attr_init failed");

  if (StackSizeInBytes) {
    if ((errnum = ::pthread_attr_setstacksize(&Attr, *StackSizeInBytes)) != 0)
      report_fatal_error("pthread_attr_setstacksize failed");
  }

  pthread_t Thread;
  if ((errnum = ::pthread_create(&Thread, &Attr, ThreadFunc, Arg)) != 0)
    report_fatal_error("pthread_create failed");

  ::pthread_attr_destroy(&Attr);
  return Thread;
}

void llvm_thread_join_impl(pthread_t Thread) {
  if (int errnum = ::pthread_join(Thread, nullptr))
    report_fatal_error("pthread_join failed");
}

void llvm_thread_detach_impl(pthread_t Thread) {
  if (int errnum = ::pthread_detach(Thread))
    report_fatal_error("pthread_detach failed");
}

llvm::thread::id llvm_thread_get_id_impl(pthread_t Thread) {
  return Thread;
}

llvm::thread::id llvm_thread_get_current_id_impl() {
  return ::pthread_self();
}

} // namespace llvm

// --- Thread naming (POSIX) ---

uint64_t llvm::get_threadid() { return uint64_t(::pthread_self()); }

uint32_t llvm::get_max_thread_name_length() { return 0; }

void llvm::set_thread_name(const Twine &Name) {
  SmallString<64> Storage;
  StringRef NameStr = Name.toNullTerminatedStringRef(Storage);
  ::pthread_setname_np(::pthread_self(), NameStr.data());
}

void llvm::get_thread_name(SmallVectorImpl<char> &Name) { Name.clear(); }

// --- Thread priority (POSIX sched + NT EcoQoS) ---

llvm::SetThreadPriorityResult
llvm::set_thread_priority(ThreadPriority Priority) {
  // EcoQoS power throttling via NT — no POSIX equivalent exists.
  // This enables the OS to schedule Background threads on efficiency cores
  // at reduced frequency (Win11+ heterogeneous scheduling).
  ThreadPowerThrottlingState_ PTS{};
  PTS.Version = THROTTLE_VERSION_;
  if (Priority == ThreadPriority::Background) {
    PTS.ControlMask = THROTTLE_EXEC_SPEED_;
    PTS.StateMask = THROTTLE_EXEC_SPEED_;
  }
  // Ignore failure — EcoQoS is best-effort.
  NtSetInformationThread(NtCurrentThread_, ThreadPowerThrottling_,
                         &PTS, sizeof(PTS));

  // Base scheduling priority via POSIX (our libc maps to NtSetInformationThread
  // ThreadBasePriority internally).
  struct sched_param Param = {};
  int Policy;
  if (Priority == ThreadPriority::Background) {
    Policy = SCHED_RR;
    Param.sched_priority = 1; // Maps to NT THREAD_PRIORITY_LOWEST
  } else {
    Policy = SCHED_OTHER;
    Param.sched_priority = 0; // Maps to NT THREAD_PRIORITY_NORMAL
  }
  if (::pthread_setschedparam(::pthread_self(), Policy, &Param) == 0)
    return SetThreadPriorityResult::SUCCESS;
  return SetThreadPriorityResult::FAILURE;
}

// --- Hardware thread count ---

static int computeHostNumHardwareThreads() {
  if (unsigned Val = std::thread::hardware_concurrency())
    return Val;
  long N = ::sysconf(_SC_NPROCESSORS_ONLN);
  return N > 0 ? static_cast<int>(N) : 1;
}

// --- CPU topology (POSIX get_mempolicy for NUMA, NT for physical cores) ---

unsigned llvm::get_cpus() {
  // NUMA topology via POSIX memory policy. get_mempolicy with
  // MPOL_F_MEMS_ALLOWED returns the set of NUMA nodes available to this
  // process — the closest POSIX equivalent to Windows processor groups.
  unsigned long nodemask[16] = {}; // Up to 1024 NUMA nodes.
  if (::get_mempolicy(nullptr, nodemask, sizeof(nodemask) * 8, nullptr,
                      MPOL_F_MEMS_ALLOWED) == 0) {
    unsigned Count = 0;
    for (unsigned long Bits : nodemask)
      Count += __builtin_popcountl(Bits);
    if (Count > 0)
      return Count;
  }
  return 1;
}

int llvm::get_physical_cores() {
  // POSIX has no API to distinguish physical from logical cores.
  // Use NT system information to count RelationProcessorCore entries.
  static int Cores = [] {
    ULONG Len = 0;
    NtQuerySystemInformation(SysLogicalProcInfoEx_, nullptr, 0, &Len);
    if (Len == 0)
      return computeHostNumHardwareThreads();

    auto *Buf = static_cast<uint8_t *>(std::calloc(1, Len));
    if (!Buf)
      return computeHostNumHardwareThreads();

    NTSTATUS St = NtQuerySystemInformation(SysLogicalProcInfoEx_,
                                           Buf, Len, &Len);
    if (St < 0) {
      std::free(Buf);
      return computeHostNumHardwareThreads();
    }

    int Count = 0;
    for (ULONG Off = 0; Off < Len;) {
      auto *E = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(Buf + Off);
      if (E->Relationship == RelationProcessorCore)
        ++Count;
      Off += E->Size;
    }

    std::free(Buf);
    return Count > 0 ? Count : computeHostNumHardwareThreads();
  }();
  return Cores;
}

// --- Thread affinity (sched_getaffinity — GNU extension, our libc) ---

llvm::BitVector llvm::get_thread_affinity_mask() {
  cpu_set_t CpuSet;
  unsigned Total = computeHostNumHardwareThreads();

  if (::sched_getaffinity(0, sizeof(CpuSet), &CpuSet) == 0) {
    llvm::BitVector V(Total, false);
    for (unsigned I = 0; I < CPU_SETSIZE && I < Total; ++I) {
      if (CPU_ISSET_S(I, sizeof(CpuSet), &CpuSet))
        V.set(I);
    }
    return V;
  }

  // Fallback: assume all processors available.
  return llvm::BitVector(Total, true);
}

// --- Thread pool strategy (POSIX NUMA + sched_setaffinity) ---

std::optional<unsigned>
llvm::ThreadPoolStrategy::compute_cpu_socket(unsigned ThreadPoolNum) const {
  unsigned NumNodes = get_cpus();
  if (NumNodes <= 1)
    return std::nullopt;

  // Distribute threads across NUMA nodes proportionally.
  unsigned ThreadsPerNode = computeHostNumHardwareThreads() / NumNodes;
  if (compute_thread_count() <= ThreadsPerNode)
    return std::nullopt;

  assert(ThreadPoolNum < compute_thread_count());
  return (ThreadPoolNum * NumNodes) / compute_thread_count();
}

void llvm::ThreadPoolStrategy::apply_thread_strategy(
    unsigned ThreadPoolNum) const {
  // Win11+ automatically distributes threads across processor groups.
  if (llvm::RunningWindows11OrGreater())
    return;

  std::optional<unsigned> Socket = compute_cpu_socket(ThreadPoolNum);
  if (!Socket)
    return;

  // Assign thread to the target processor group via sched_setaffinity.
  // Bits [group*64, group*64+63] in cpu_set_t map to Windows processor groups.
  cpu_set_t CpuSet;
  CPU_ZERO(&CpuSet);
  unsigned GroupBase = *Socket * (sizeof(KAFFINITY) * 8);
  for (unsigned I = 0; I < sizeof(KAFFINITY) * 8; ++I)
    CPU_SET_S(GroupBase + I, sizeof(CpuSet), &CpuSet);
  ::sched_setaffinity(0, sizeof(CpuSet), &CpuSet);
}

#else // !LLVM_RUNTIME_POSIX

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/thread.h"

#include "llvm/Support/Windows/WindowsSupport.h"
#include <process.h>

#include <bitset>

// Windows will at times define MemoryFence.
#ifdef MemoryFence
#undef MemoryFence
#endif

namespace llvm {
HANDLE
llvm_execute_on_thread_impl(unsigned(__stdcall *ThreadFunc)(void *), void *Arg,
                            std::optional<unsigned> StackSizeInBytes) {
  HANDLE hThread = (HANDLE)::_beginthreadex(NULL, StackSizeInBytes.value_or(0),
                                            ThreadFunc, Arg, 0, NULL);

  if (!hThread)
    ReportLastErrorFatal("_beginthreadex failed");

  return hThread;
}

void llvm_thread_join_impl(HANDLE hThread) {
  if (::WaitForSingleObject(hThread, INFINITE) == WAIT_FAILED)
    ReportLastErrorFatal("WaitForSingleObject failed");
  if (::CloseHandle(hThread) == FALSE)
    ReportLastErrorFatal("CloseHandle failed");
}

void llvm_thread_detach_impl(HANDLE hThread) {
  if (::CloseHandle(hThread) == FALSE)
    ReportLastErrorFatal("CloseHandle failed");
}

DWORD llvm_thread_get_id_impl(HANDLE hThread) { return ::GetThreadId(hThread); }

DWORD llvm_thread_get_current_id_impl() { return ::GetCurrentThreadId(); }

} // namespace llvm

uint64_t llvm::get_threadid() { return uint64_t(::GetCurrentThreadId()); }

uint32_t llvm::get_max_thread_name_length() { return 0; }

#if defined(_MSC_VER)
static void SetThreadName(DWORD Id, LPCSTR Name) {
  constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push, 8)
  struct THREADNAME_INFO {
    DWORD dwType;     // Must be 0x1000.
    LPCSTR szName;    // Pointer to thread name
    DWORD dwThreadId; // Thread ID (-1 == current thread)
    DWORD dwFlags;    // Reserved.  Do not use.
  };
#pragma pack(pop)

  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = Name;
  info.dwThreadId = Id;
  info.dwFlags = 0;

  __try {
    ::RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR),
                     (ULONG_PTR *)&info);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
#endif

void llvm::set_thread_name(const Twine &Name) {
#if defined(_MSC_VER)
  // Make sure the input is null terminated.
  SmallString<64> Storage;
  StringRef NameStr = Name.toNullTerminatedStringRef(Storage);
  SetThreadName(::GetCurrentThreadId(), NameStr.data());
#endif
}

void llvm::get_thread_name(SmallVectorImpl<char> &Name) {
  // "Name" is not an inherent property of a thread on Windows.  In fact, when
  // you "set" the name, you are only firing a one-time message to a debugger
  // which it interprets as a program setting its threads' name.  We may be
  // able to get fancy by creating a TLS entry when someone calls
  // set_thread_name so that subsequent calls to get_thread_name return this
  // value.
  Name.clear();
}

llvm::SetThreadPriorityResult
llvm::set_thread_priority(ThreadPriority Priority) {
#ifdef THREAD_POWER_THROTTLING_CURRENT_VERSION
  HMODULE kernelM = llvm::sys::windows::loadSystemModuleSecure(L"kernel32.dll");
  if (kernelM) {
    // SetThreadInformation is only available on Windows 8 and later. Since we
    // still support compilation on Windows 7, we load the function dynamically.
    typedef BOOL(WINAPI * SetThreadInformation_t)(
        HANDLE hThread, THREAD_INFORMATION_CLASS ThreadInformationClass,
        _In_reads_bytes_(ThreadInformationSize) PVOID ThreadInformation,
        ULONG ThreadInformationSize);
    static const auto pfnSetThreadInformation =
        (SetThreadInformation_t)::GetProcAddress(kernelM,
                                                 "SetThreadInformation");
    if (pfnSetThreadInformation) {
      auto setThreadInformation = [](ULONG ControlMaskAndStateMask) {
        THREAD_POWER_THROTTLING_STATE state{};
        state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = ControlMaskAndStateMask;
        state.StateMask = ControlMaskAndStateMask;
        return pfnSetThreadInformation(
            ::GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
      };

      // Use EcoQoS for ThreadPriority::Background available (running on most
      // efficent cores at the most efficient cpu frequency):
      // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadinformation
      // https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service
      setThreadInformation(Priority == ThreadPriority::Background
                               ? THREAD_POWER_THROTTLING_EXECUTION_SPEED
                               : 0);
    }
  }
#endif

  // https://docs.microsoft.com/en-us/windows/desktop/api/processthreadsapi/nf-processthreadsapi-setthreadpriority
  // Begin background processing mode. The system lowers the resource scheduling
  // priorities of the thread so that it can perform background work without
  // significantly affecting activity in the foreground.
  // End background processing mode. The system restores the resource scheduling
  // priorities of the thread as they were before the thread entered background
  // processing mode.
  //
  // FIXME: consider THREAD_PRIORITY_BELOW_NORMAL for Low
  return SetThreadPriority(GetCurrentThread(),
                           Priority != ThreadPriority::Default
                               ? THREAD_MODE_BACKGROUND_BEGIN
                               : THREAD_MODE_BACKGROUND_END)
             ? SetThreadPriorityResult::SUCCESS
             : SetThreadPriorityResult::FAILURE;
}

struct ProcessorGroup {
  unsigned ID;
  unsigned AllThreads;
  unsigned UsableThreads;
  unsigned ThreadsPerCore;
  uint64_t Affinity;

  unsigned useableCores() const {
    return std::max(1U, UsableThreads / ThreadsPerCore);
  }
};

template <typename F>
static bool IterateProcInfo(LOGICAL_PROCESSOR_RELATIONSHIP Relationship, F Fn) {
  DWORD Len = 0;
  BOOL R = ::GetLogicalProcessorInformationEx(Relationship, NULL, &Len);
  if (R || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    return false;

  auto *Info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)calloc(1, Len);
  R = ::GetLogicalProcessorInformationEx(Relationship, Info, &Len);
  if (R) {
    auto *End =
        (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((uint8_t *)Info + Len);
    for (auto *Curr = Info; Curr < End;
         Curr = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((uint8_t *)Curr +
                                                            Curr->Size)) {
      if (Curr->Relationship != Relationship)
        continue;
      Fn(Curr);
    }
  }
  free(Info);
  return true;
}

static std::optional<std::vector<USHORT>> getActiveGroups() {
  USHORT Count = 0;
  if (::GetProcessGroupAffinity(GetCurrentProcess(), &Count, nullptr))
    return std::nullopt;

  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    return std::nullopt;

  std::vector<USHORT> Groups;
  Groups.resize(Count);
  if (!::GetProcessGroupAffinity(GetCurrentProcess(), &Count, Groups.data()))
    return std::nullopt;

  return Groups;
}

static llvm::ArrayRef<ProcessorGroup> getProcessorGroups() {
  auto computeGroups = []() {
    llvm::SmallVector<ProcessorGroup, 4> Groups;

    auto HandleGroup = [&](SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *ProcInfo) {
      GROUP_RELATIONSHIP &El = ProcInfo->Group;
      for (unsigned J = 0; J < El.ActiveGroupCount; ++J) {
        ProcessorGroup G;
        G.ID = Groups.size();
        G.AllThreads = El.GroupInfo[J].MaximumProcessorCount;
        G.UsableThreads = El.GroupInfo[J].ActiveProcessorCount;
        assert(G.UsableThreads <= 64);
        G.Affinity = El.GroupInfo[J].ActiveProcessorMask;
        Groups.push_back(G);
      }
    };

    if (!IterateProcInfo(RelationGroup, HandleGroup))
      return std::vector<ProcessorGroup>();

    auto HandleProc = [&](SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *ProcInfo) {
      PROCESSOR_RELATIONSHIP &El = ProcInfo->Processor;
      assert(El.GroupCount == 1);
      unsigned NumHyperThreads = 1;
      // If the flag is set, each core supports more than one hyper-thread.
      if (El.Flags & LTP_PC_SMT)
        NumHyperThreads = std::bitset<64>(El.GroupMask[0].Mask).count();
      unsigned I = El.GroupMask[0].Group;
      Groups[I].ThreadsPerCore = NumHyperThreads;
    };

    if (!IterateProcInfo(RelationProcessorCore, HandleProc))
      return std::vector<ProcessorGroup>();

    auto ActiveGroups = getActiveGroups();
    if (!ActiveGroups)
      return std::vector<ProcessorGroup>();

    // If there's an affinity mask set, assume the user wants to constrain the
    // current process to only a single CPU group. On Windows, it is not
    // possible for affinity masks to cross CPU group boundaries.
    DWORD_PTR ProcessAffinityMask = 0, SystemAffinityMask = 0;
    if (::GetProcessAffinityMask(GetCurrentProcess(), &ProcessAffinityMask,
                                 &SystemAffinityMask)) {

      if (ProcessAffinityMask != SystemAffinityMask) {
        if (llvm::RunningWindows11OrGreater() && ActiveGroups->size() > 1) {
          // The process affinity mask is spurious, due to an OS bug, ignore it.
          return std::vector<ProcessorGroup>(Groups.begin(), Groups.end());
        }

        assert(ActiveGroups->size() == 1 &&
               "When an affinity mask is set, the process is expected to be "
               "assigned to a single processor group!");

        unsigned CurrentGroupID = (*ActiveGroups)[0];
        ProcessorGroup NewG{Groups[CurrentGroupID]};
        NewG.Affinity = ProcessAffinityMask;
        NewG.UsableThreads = llvm::popcount(ProcessAffinityMask);
        Groups.clear();
        Groups.push_back(NewG);
      }
    }
    return std::vector<ProcessorGroup>(Groups.begin(), Groups.end());
  };
  static auto Groups = computeGroups();
  return llvm::ArrayRef<ProcessorGroup>(Groups);
}

template <typename R, typename UnaryPredicate>
static unsigned aggregate(R &&Range, UnaryPredicate P) {
  unsigned I{};
  for (const auto &It : Range)
    I += P(It);
  return I;
}

int llvm::get_physical_cores() {
  static unsigned Cores =
      aggregate(getProcessorGroups(), [](const ProcessorGroup &G) {
        return G.UsableThreads / G.ThreadsPerCore;
      });
  return Cores;
}

static int computeHostNumHardwareThreads() {
  static unsigned Threads =
      aggregate(getProcessorGroups(),
                [](const ProcessorGroup &G) { return G.UsableThreads; });
  return Threads;
}

// Finds the proper CPU socket where a thread number should go. Returns
// 'std::nullopt' if the thread shall remain on the actual CPU socket.
std::optional<unsigned>
llvm::ThreadPoolStrategy::compute_cpu_socket(unsigned ThreadPoolNum) const {
  ArrayRef<ProcessorGroup> Groups = getProcessorGroups();
  // Only one CPU socket in the system or process affinity was set, no need to
  // move the thread(s) to another CPU socket.
  if (Groups.size() <= 1)
    return std::nullopt;

  // We ask for less threads than there are hardware threads per CPU socket, no
  // need to dispatch threads to other CPU sockets.
  unsigned MaxThreadsPerSocket =
      UseHyperThreads ? Groups[0].UsableThreads : Groups[0].useableCores();
  if (compute_thread_count() <= MaxThreadsPerSocket)
    return std::nullopt;

  assert(ThreadPoolNum < compute_thread_count() &&
         "The thread index is not within thread strategy's range!");

  // Assumes the same number of hardware threads per CPU socket.
  return (ThreadPoolNum * Groups.size()) / compute_thread_count();
}

// Assign the current thread to a more appropriate CPU socket or CPU group
void llvm::ThreadPoolStrategy::apply_thread_strategy(
    unsigned ThreadPoolNum) const {

  // After Windows 11 and Windows Server 2022, let the OS do the scheduling,
  // since a process automatically gains access to all processor groups.
  if (llvm::RunningWindows11OrGreater())
    return;

  std::optional<unsigned> Socket = compute_cpu_socket(ThreadPoolNum);
  if (!Socket)
    return;
  ArrayRef<ProcessorGroup> Groups = getProcessorGroups();
  GROUP_AFFINITY Affinity{};
  Affinity.Group = Groups[*Socket].ID;
  Affinity.Mask = Groups[*Socket].Affinity;
  SetThreadGroupAffinity(GetCurrentThread(), &Affinity, nullptr);
}

llvm::BitVector llvm::get_thread_affinity_mask() {
  GROUP_AFFINITY Affinity{};
  GetThreadGroupAffinity(GetCurrentThread(), &Affinity);

  static unsigned All =
      aggregate(getProcessorGroups(),
                [](const ProcessorGroup &G) { return G.AllThreads; });

  unsigned StartOffset =
      aggregate(getProcessorGroups(), [&](const ProcessorGroup &G) {
        return G.ID < Affinity.Group ? G.AllThreads : 0;
      });

  llvm::BitVector V;
  V.resize(All);
  for (unsigned I = 0; I < sizeof(KAFFINITY) * 8; ++I) {
    if ((Affinity.Mask >> I) & 1)
      V.set(StartOffset + I);
  }
  return V;
}

unsigned llvm::get_cpus() { return getProcessorGroups().size(); }

#endif // LLVM_RUNTIME_POSIX
