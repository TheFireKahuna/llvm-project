// Exported user types: virtual bases, RTTI, exceptions and allocation ownership.
// RUN: %clangxx_crt_dll -std=c++17 -O0 -UNDEBUG -DBUILD_DLL %s -o %t.dll -Wl,/implib:%t.lib
// RUN: %clangxx_crt_main -std=c++17 -O0 -UNDEBUG %s %t.lib -o %t.exe
// RUN: %run %t.exe
// RUN: %clangxx_crt_dll -std=c++23 -O2 -flto=thin -mguard=cf -UNDEBUG -DBUILD_DLL %s -o %t.opt.dll -Wl,/implib:%t.opt.lib -Wl,/guard:cf
// RUN: %clangxx_crt_main -std=c++23 -O2 -flto=thin -mguard=cf -UNDEBUG %s %t.opt.lib -o %t.opt.exe -Wl,/guard:cf
// RUN: %run %t.opt.exe
// REQUIRES: windows, crt

#include "Inputs/itanium_dll_contract.h"
#include <assert.h>
#include <exception>
#include <new>
#include <stdint.h>
#include <string>
#include <thread>
#include <typeinfo>

#ifdef BUILD_DLL
static int destroyed;
Root::~Root() = default;
Derived::~Derived() { ++destroyed; }
int Left::left() const { return 11; }
int Right::right() const { return 12; }
Error::Error(int v) : std::runtime_error("user exception"), value(v) {}
Error::~Error() = default;
Root *make_object() { return new Derived; }
void throw_error() { throw Error(42); }
int catch_callback(void (*callback)()) {
  try {
    callback();
  } catch (const Error &e) {
    return e.value;
  }
  return -1;
}
int destruction_count() { return destroyed; }
Block *make_block() { return new Block{}; }
void delete_block(Block *block) { delete block; }
bool check_new_handler(void (*handler)()) {
  return std::get_new_handler() == handler;
}
void fail_allocation() {
  volatile size_t size = static_cast<size_t>(-1);
  void *memory = ::operator new(size);
  ::operator delete(memory);
  assert(false);
}
#else
static void from_host() { throw Error(43); }
static int allocationFailures;
static void new_handler() {
  ++allocationFailures;
  throw std::bad_alloc();
}
int main() {
  Root *root = make_object();
  auto *left = dynamic_cast<Left *>(root);
  auto *right = dynamic_cast<Right *>(left);
  auto *derived = dynamic_cast<Derived *>(right);
  assert(left && right && derived && derived->value == 42);
  assert(left->left() == 11 && right->right() == 12);
  assert(static_cast<Root *>(left) == static_cast<Root *>(right));
  assert(dynamic_cast<void *>(root) == derived &&
         typeid(*root) == typeid(Derived));
  delete root;
  assert(destruction_count() == 1);

  std::exception_ptr saved;
  try {
    throw_error();
  } catch (const Error &e) {
    assert(e.value == 42 && std::string(e.what()) == "user exception");
    saved = std::current_exception();
  }
  assert(saved);
  std::thread([saved] {
    try {
      std::rethrow_exception(saved);
    } catch (const Error &e) {
      assert(e.value == 42 && typeid(e) == typeid(Error));
      return;
    }
    assert(false);
  }).join();
  saved = nullptr;
  assert(catch_callback(from_host) == 43);

  auto previous = std::set_new_handler(new_handler);
  assert(check_new_handler(new_handler));
  try {
    fail_allocation();
  } catch (const std::bad_alloc &) {
    assert(allocationFailures == 1);
  }
  assert(allocationFailures == 1);
  volatile size_t impossible = static_cast<size_t>(-1);
  assert(::operator new(impossible, std::nothrow) == nullptr);
  assert(allocationFailures == 2);
  std::set_new_handler(previous);
  Block *block = make_block();
  assert(reinterpret_cast<uintptr_t>(block) % alignof(Block) == 0);
  for (auto byte : block->bytes)
    assert(byte == 0);
  delete block;
  block = new Block{};
  assert(reinterpret_cast<uintptr_t>(block) % alignof(Block) == 0);
  delete_block(block);
}
#endif
