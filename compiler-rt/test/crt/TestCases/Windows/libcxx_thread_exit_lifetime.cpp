// libc++'s thread-exit notifications must follow language TLS destruction.
// RUN: %clangxx_crt_main -std=c++17 -O2 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// REQUIRES: windows, crt

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdio.h>
#include <thread>

struct Object {
  std::atomic<int> &destroyed;
  ~Object() { destroyed.fetch_add(1); }
};

// Hold the language TLS destructor open so a fast destructor cannot hide an
// early future notification or mutex unlock. The timeout also diagnoses a
// missing TLS callback without leaving the observer blocked indefinitely.
struct ExitGate {
  std::promise<void> entered;
  std::promise<void> release;
  std::future<void> released = release.get_future();
};

struct BlockingObject {
  std::atomic<int> &destroyed;
  ExitGate &gate;
  ~BlockingObject() {
    gate.entered.set_value();
    gate.released.wait();
    destroyed.fetch_add(1);
  }
};

int main() {
  std::atomic<int> destroyed(0);
  std::thread([&] { thread_local Object object{destroyed}; }).join();
  printf("after join = %d\n", destroyed.load());

  std::promise<void> promise;
  auto future = promise.get_future();
  ExitGate futureGate;
  auto futureDestructor = futureGate.entered.get_future();
  std::thread producer([&] {
    thread_local BlockingObject object{destroyed, futureGate};
    promise.set_value_at_thread_exit();
  });
  bool entered = futureDestructor.wait_for(std::chrono::seconds(5)) ==
                 std::future_status::ready;
  printf("future destructor entered = %d\n", entered);
  printf("future ready during destruction = %d\n",
         future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  futureGate.release.set_value();
  producer.join();
  future.get();
  printf("after future join = %d\n", destroyed.load());

  std::mutex mutex;
  std::condition_variable cv;
  ExitGate notifyGate;
  auto notifyDestructor = notifyGate.entered.get_future();
  std::thread notifier([&] {
    thread_local BlockingObject object{destroyed, notifyGate};
    std::unique_lock<std::mutex> workerLock(mutex);
    std::notify_all_at_thread_exit(cv, std::move(workerLock));
  });
  entered = notifyDestructor.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready;
  printf("notification destructor entered = %d\n", entered);
  bool unlocked = mutex.try_lock();
  printf("notification mutex unlocked during destruction = %d\n", unlocked);
  if (unlocked)
    mutex.unlock();
  notifyGate.release.set_value();
  notifier.join();
  printf("after notification join = %d\n", destroyed.load());
}

// CHECK: after join = 1
// CHECK-NEXT: future destructor entered = 1
// CHECK-NEXT: future ready during destruction = 0
// CHECK-NEXT: after future join = 2
// CHECK-NEXT: notification destructor entered = 1
// CHECK-NEXT: notification mutex unlocked during destruction = 0
// CHECK-NEXT: after notification join = 3
