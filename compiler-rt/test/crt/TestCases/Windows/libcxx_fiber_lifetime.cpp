// Deleting a fiber must not release notifications scheduled for thread exit.
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// REQUIRES: windows, crt

#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <windows.h>

struct State {
  std::atomic<int> destroyed{0};
  std::mutex mutex;
  std::condition_variable cv;
  std::promise<void> promise;
  void *parent;
};

struct Object {
  std::atomic<int> &destroyed;
  ~Object() { destroyed.fetch_add(1); }
};

static void WINAPI runFiber(void *arg) {
  auto &state = *static_cast<State *>(arg);
  thread_local Object object{state.destroyed};
  std::unique_lock<std::mutex> lock(state.mutex);
  std::notify_all_at_thread_exit(state.cv, std::move(lock));
  state.promise.set_value_at_thread_exit();
  SwitchToFiber(state.parent);
  // Returning from a fiber procedure would terminate the underlying thread.
  assert(false);
}

int main() {
  State state;
  auto future = state.promise.get_future();
  std::promise<void> deleted, resume;
  auto deletion = deleted.get_future();
  auto resumed = resume.get_future();
  std::thread worker([&] {
    state.parent = ConvertThreadToFiber(nullptr);
    assert(state.parent);
    void *fiber = CreateFiber(0, runFiber, &state);
    assert(fiber);
    SwitchToFiber(fiber);
    DeleteFiber(fiber);

    deleted.set_value();
    resumed.wait();
    assert(ConvertFiberToThread());
  });
  assert(deletion.wait_for(std::chrono::seconds(5)) ==
         std::future_status::ready);
  printf("destroyed after fiber deletion = %d\n", state.destroyed.load());
  printf("future ready after fiber deletion = %d\n",
         future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  bool unlocked = state.mutex.try_lock();
  printf("mutex unlocked after fiber deletion = %d\n", unlocked);
  if (unlocked)
    state.mutex.unlock();
  resume.set_value();
  worker.join();
  printf("destroyed after join = %d\n", state.destroyed.load());
  printf("future ready after join = %d\n",
         future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  unlocked = state.mutex.try_lock();
  printf("mutex unlocked after join = %d\n", unlocked);
  if (unlocked)
    state.mutex.unlock();
}

// CHECK: destroyed after fiber deletion = 0
// CHECK-NEXT: future ready after fiber deletion = 0
// CHECK-NEXT: mutex unlocked after fiber deletion = 0
// CHECK-NEXT: destroyed after join = 1
// CHECK-NEXT: future ready after join = 1
// CHECK-NEXT: mutex unlocked after join = 1
