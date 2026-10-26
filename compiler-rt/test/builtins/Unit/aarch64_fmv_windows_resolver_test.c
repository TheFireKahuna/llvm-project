// REQUIRES: aarch64-target-arch, windows
// REQUIRES: librt_has_aarch64
// REQUIRES: native-run
// RUN: %clang_builtins %s %librt -o %t && %run %t

// The resolver of a multiversioned function fills the CPU features itself,
// since it can run before the runtime's constructor that otherwise does,
// here from a constructor of a lower priority.

#include "cpu_model/AArch64CPUFeatures.inc"

extern struct {
  unsigned long long features;
} __aarch64_cpu_features;

void __init_cpu_features_resolver(void);

static unsigned long long features_before_constructor;

__attribute__((constructor(1))) static void before_runtime_constructor(void) {
  __init_cpu_features_resolver();
  features_before_constructor = __aarch64_cpu_features.features;
}

int main(void) { return !((features_before_constructor >> FEAT_INIT) & 1); }
