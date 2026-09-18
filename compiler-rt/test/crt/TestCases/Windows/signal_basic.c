// Test basic signal handling.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t g_signal_received = 0;

void sigint_handler(int sig) {
  g_signal_received = sig;
  printf("Signal handler called with sig=%d\n", sig);
}

int main(void) {
  // CHECK: Signal test
  printf("Signal test\n");

  // Install SIGINT handler.
  void (*old_handler)(int) = signal(SIGINT, sigint_handler);
  // CHECK: signal installed = 1
  printf("signal installed = %d\n", old_handler != SIG_ERR);

  // Raise signal to ourselves.
  // CHECK: raising SIGINT
  printf("raising SIGINT\n");
  raise(SIGINT);

  // CHECK: Signal handler called with sig=2
  // CHECK: signal received = 2
  printf("signal received = %d\n", (int)g_signal_received);

  // Restore default handler.
  signal(SIGINT, SIG_DFL);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
