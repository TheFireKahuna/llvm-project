//==- llvm/Support/Windows/NTPOSIXJobserver.inc -----------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Windows-specific NTPOSIX jobserver client.
//
// NTPOSIX must interoperate with both GNU make's POSIX auth forms
// (`R,W` and `fifo:PATH`) and GNU make's Windows named semaphore auth.
// Pipe/FIFO modes use the POSIX C layer. Semaphore mode also stays in the
// POSIX layer by opening the semaphore name via sem_open(); slashless names
// are handled by the NTPOSIX sem_open extension in llvm-libc.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <semaphore.h>
#include <string.h>
#include <unistd.h>

namespace {
bool areFdsValid(int ReadFD, int WriteFD) {
  if (ReadFD == -1 || WriteFD == -1)
    return false;
  return ::fcntl(ReadFD, F_GETFD) != -1 && ::fcntl(WriteFD, F_GETFD) != -1;
}
} // namespace

namespace llvm {
JobserverClientImpl::JobserverClientImpl(const JobserverConfig &Config) {
  Mode = Config.TheMode;

  switch (Mode) {
  case JobserverConfig::PosixPipe: {
    int NewReadFD = ::dup(Config.PipeFDs.Read);
    if (NewReadFD < 0)
      return;
    int NewWriteFD = ::dup(Config.PipeFDs.Write);
    if (NewWriteFD < 0) {
      ::close(NewReadFD);
      return;
    }
    if (::fcntl(NewReadFD, F_SETFD, FD_CLOEXEC) == -1 ||
        ::fcntl(NewWriteFD, F_SETFD, FD_CLOEXEC) == -1) {
      ::close(NewReadFD);
      ::close(NewWriteFD);
      return;
    }
    int Flags = ::fcntl(NewReadFD, F_GETFL, 0);
    if (Flags == -1 ||
        ::fcntl(NewReadFD, F_SETFL, Flags | O_NONBLOCK) == -1) {
      ::close(NewReadFD);
      ::close(NewWriteFD);
      return;
    }
    ReadFD = NewReadFD;
    WriteFD = NewWriteFD;
    break;
  }
  case JobserverConfig::PosixFifo:
    ReadFD = ::open(Config.Path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (ReadFD < 0) {
      if (ReadFD >= 0)
        ::close(ReadFD);
      ReadFD = -1;
      return;
    }
    FifoPath = Config.Path;
    WriteFD = -1;
    break;
  case JobserverConfig::Win32Semaphore:
    Semaphore = (void *)::sem_open(Config.Path.c_str(), 0);
    if (Semaphore == (void *)SEM_FAILED) {
      Semaphore = nullptr;
      return;
    }
    break;
  default:
    return;
  }

  IsInitialized = true;

  SmallVector<JobSlot, 8> Slots;
  while (true) {
    auto S = tryAcquire();
    if (!S.isValid())
      break;
    Slots.push_back(std::move(S));
  }
  NumJobs = Slots.size();
  assert(NumJobs >= 1 && "Invalid number of jobs");
  for (auto &S : Slots)
    release(std::move(S));
}

JobserverClientImpl::~JobserverClientImpl() {
  if (ReadFD >= 0)
    ::close(ReadFD);
  if (WriteFD >= 0)
    ::close(WriteFD);
  if (Semaphore != nullptr)
    ::sem_close((sem_t *)Semaphore);
}

JobSlot JobserverClientImpl::tryAcquire() {
  if (!IsInitialized)
    return JobSlot();

  if (HasImplicitSlot.exchange(false, std::memory_order_acquire))
    return JobSlot::createImplicit();

  switch (Mode) {
  case JobserverConfig::PosixPipe:
  case JobserverConfig::PosixFifo: {
    char Token;
    ssize_t Ret;
    do {
      Ret = ::read(ReadFD, &Token, 1);
    } while (Ret < 0 && errno == EINTR);

    if (Ret == 1)
      return JobSlot::createExplicit(static_cast<uint8_t>(Token));
    return JobSlot();
  }
  case JobserverConfig::Win32Semaphore:
    if (::sem_trywait((sem_t *)Semaphore) == 0)
      return JobSlot::createExplicit(1);
    return JobSlot();
  default:
    return JobSlot();
  }
}

void JobserverClientImpl::release(JobSlot Slot) {
  if (!IsInitialized || !Slot.isValid())
    return;

  if (Slot.isImplicit()) {
    [[maybe_unused]] bool WasAlreadyReleased =
        HasImplicitSlot.exchange(true, std::memory_order_release);
    assert(!WasAlreadyReleased && "Implicit slot released twice");
    return;
  }

  switch (Mode) {
  case JobserverConfig::PosixPipe:
  case JobserverConfig::PosixFifo: {
    uint8_t Token = Slot.getExplicitValue();
    if (WriteFD < 0) {
      WriteFD = ::open(FifoPath.c_str(), O_WRONLY | O_CLOEXEC);
      if (WriteFD < 0)
        return;
    }

    ssize_t Written;
    do {
      Written = ::write(WriteFD, &Token, 1);
    } while (Written < 0 && errno == EINTR);
    return;
  }
  case JobserverConfig::Win32Semaphore:
    (void)::sem_post((sem_t *)Semaphore);
    return;
  default:
    return;
  }
}
} // namespace llvm
