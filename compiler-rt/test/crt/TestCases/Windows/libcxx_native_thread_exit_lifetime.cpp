// Native-created threads must initialize libc++ state on first use, and make
// futures ready after language TLS destruction, including across fiber changes.
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <atomic>
#include <chrono>
#include <future>
#include <process.h>
#include <windows.h>

struct State {
  std::atomic<bool> destroyed{false};
  std::promise<void> promise;
  void *parent = nullptr;
  bool fiber;
};
struct Object {
  State &state;
  ~Object() { state.destroyed.store(true); }
};
static void produce(State &state) {
  thread_local Object object{state};
  // Deliberately no notify_all_at_thread_exit call to initialize library state.
  state.promise.set_value_at_thread_exit();
}
static void WINAPI fiber(void *argument) {
  auto &state = *static_cast<State *>(argument);
  produce(state);
  SwitchToFiber(state.parent);
  assert(false);
}
template <class Result> static Result __stdcall worker(void *argument) {
  auto &state = *static_cast<State *>(argument);
  if (state.fiber) {
    state.parent = ConvertThreadToFiber(nullptr);
    assert(state.parent);
    void *child = CreateFiber(0, fiber, &state);
    assert(child);
    SwitchToFiber(child);
    DeleteFiber(child);
    assert(!state.destroyed.load());
    assert(ConvertFiberToThread());
  } else {
    produce(state);
  }
  return 0;
}

int main() {
  for (bool useBeginthread : {false, true}) {
    for (bool useFiber : {false, true}) {
      State state;
      state.fiber = useFiber;
      auto future = state.promise.get_future();
      HANDLE thread =
          useBeginthread
              ? reinterpret_cast<HANDLE>(_beginthreadex(
                    nullptr, 0, worker<unsigned>, &state, 0, nullptr))
              : CreateThread(nullptr, 0, worker<DWORD>, &state, 0, nullptr);
      assert(thread);
      assert(future.wait_for(std::chrono::seconds(5)) ==
             std::future_status::ready);
      assert(state.destroyed.load());
      assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
      DWORD code;
      assert(GetExitCodeThread(thread, &code) && code == 0);
      assert(CloseHandle(thread));
    }
  }
}
