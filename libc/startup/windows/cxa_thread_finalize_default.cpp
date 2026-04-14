// Weak defaults for __cxa_thread_finalize and __cxa_thread_finalize_dso_unload
// — no-ops when the full thread infrastructure (thread.cpp) isn't linked.
// The strong definitions in thread.cpp override these when libc is fully
// linked. Same pattern as libc/src/stdlib/exit.cpp.
extern "C" [[gnu::weak]] void __cxa_thread_finalize(void *) {}
extern "C" [[gnu::weak]] void __cxa_thread_finalize_dso_unload(void *) {}
