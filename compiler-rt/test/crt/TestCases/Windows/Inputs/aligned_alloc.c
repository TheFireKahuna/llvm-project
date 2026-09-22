#include <stdlib.h>

__declspec(dllexport) void *allocate_in_c(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}

__declspec(dllexport) void free_in_c(void *p) { free(p); }
