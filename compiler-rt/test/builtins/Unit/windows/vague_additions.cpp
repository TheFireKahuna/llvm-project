// DEFINE: %{cc} = %clang -fno-exceptions -fno-threadsafe-statics
// RUN: rm -f %t-d.dll.lib %t-f.dll.lib %t-o.dll.lib
// RUN: %{cc} -shared -DCOUNT %s -o %t-cnt.dll
// RUN: %{cc} -shared -DKEEPER %s %t-cnt.dll.lib -o %t-s.dll
// RUN: %{cc} -shared -DNEWER=d %s %t-cnt.dll.lib -o %t-d.dll
// RUN: %{cc} -shared -DNEWER=f %s %t-cnt.dll.lib -o %t-f.dll
// RUN: %clang -fno-exceptions --target=x86_64-pc-windows-msvc -fuse-ld=lld \
// RUN:   -nostdlib -Wl,-noentry -shared -DFOREIGN %s -o %t-g.dll
// RUN: %{cc} %s %t-d.dll.lib %t-f.dll.lib %t-s.dll.lib %t-g.lib \
// RUN:   %t-cnt.dll.lib -o %t.exe
// RUN: %run %t.exe | FileCheck --check-prefix=FIRST %s
// RUN: %{cc} -shared -DNEWER=d -DSECOND %s %t-cnt.dll.lib -o %t-d.dll 2>&1 | \
// RUN:   FileCheck --check-prefix=WARN %s
// RUN: %run %t.exe | FileCheck --check-prefix=D %s
// RUN: %{cc} -shared -DNEWER=f -DSECOND %s %t-cnt.dll.lib -o %t-f.dll
// RUN: %run %t.exe | FileCheck --check-prefix=F %s

// RUN: %{cc} -shared -DKEEPER %s %t-cnt.dll.lib -Wl,-offers-epoch -o %t-o.dll
// RUN: %{cc} -shared -DUSER %s %t-o.dll.lib -o %t-u.dll
// RUN: %{cc} -DEPOCH %s %t-u.dll.lib -o %t-epoch.exe
// RUN: %run %t-epoch.exe | FileCheck --check-prefix=EPOCH %s
// RUN: %{cc} -shared -DKEEPER -DSECOND %s %t-cnt.dll.lib -Wl,-offers-epoch \
// RUN:   -o %t-o.dll
// RUN: %exit_status %t-epoch.exe | FileCheck --check-prefix=REFUSED %s

// REQUIRES: target={{x86_64-.*}}

// An executable runs beside d.dll and f.dll as its link saw them, keeping
// nothing, and s.dll, which keeps vc and vd. A newer d.dll, and then a newer
// f.dll, define both, which their baseline, the release that each replaces,
// did not: each one's link warns, and its fill yields each to s.dll's, the
// instance that the process settled, which neither newer DLL takes from the
// other before that one has yielded. The newer d.dll also keeps n::vx, n::vg
// and n::vh, which only g.dll, of another producer, keeps and exports by
// their mangled names: it takes g.dll's vx, and its vg, whose guard g.dll
// exports too, so that vg's initializer does not run again; it keeps its own
// vh, whose guard g.dll does not export.
//
// Under -offers-epoch, o.dll exports its epoch, which u.dll, a DLL that
// offers an entity and imports from o.dll, imports: a newer o.dll that keeps
// more renames it, and the executable that loads u.dll does not start: the
// loader finds no entry point of that name.

// FIRST: d: vc none vd none, f: vc none vd none, inits 1

// WARN-DAG: warning: {{.*}}d.dll offers n::vg, which its baseline does not export
// WARN-DAG: warning: {{.*}}d.dll offers n::vh, which its baseline does not export
// WARN-DAG: warning: {{.*}}d.dll offers n::vx, which its baseline does not export
// WARN-DAG: warning: {{.*}}d.dll offers vc, which its baseline does not export
// WARN-DAG: warning: {{.*}}d.dll offers vd, which its baseline does not export

// D:      d: vc one vd one, f: vc none vd none, inits 2
// D-NEXT: foreign: vx one vg one 9, vh split, inits 2
// F:      d: vc one vd one, f: vc one vd one, inits 2
// F-NEXT: foreign: vx one vg one 9, vh split, inits 2

// EPOCH: epoch: 3 7
// REFUSED: exit code 3221225785

#define EXPORT extern "C" __declspec(dllexport)
#define DEFAULT __attribute__((visibility("default")))

#ifdef COUNT
static int Count;
EXPORT int init(void) { return ++Count; }
EXPORT int inits(void) { return Count; }
#elif defined(FOREIGN)
// Another producer's DLL, which has no offer table and exports n::vx, n::vg,
// which its guard says is initialized, and n::vh by their mangled names.
EXPORT int vx __asm__("_ZN1n2vxE") = 7;
EXPORT int vg __asm__("_ZN1n2vgE") = 9;
EXPORT long long vgGuard __asm__("_ZGVN1n2vgE") = 1;
EXPORT int vh __asm__("_ZN1n2vhE") = 11;
EXPORT int *gX(void) { return &vx; }
EXPORT int *gG(void) { return &vg; }
EXPORT int *gH(void) { return &vh; }
#else
extern "C" __declspec(dllimport) int init(void);
extern "C" __declspec(dllimport) int inits(void);
#  if defined(KEEPER) || defined(NEWER) && defined(SECOND)
inline int vc = 7;
inline int vd = init();
#  endif
#  ifdef KEEPER
EXPORT const int *sC(void) { return &vc; }
EXPORT const int *sD(void) { return &vd; }
#    ifdef SECOND
inline int vn = 5;
EXPORT const int *sN(void) { return &vn; }
#    endif
#  elif defined(USER)
extern "C" __declspec(dllimport) const int *sC(void);
inline int vq = 3;
EXPORT int user(void) { return vq * 10 + *sC(); }
#  elif defined(EPOCH)
#    include <stdio.h>
extern "C" __declspec(dllimport) int user(void);
int main() {
  int U = user();
  printf("epoch: %d %d\n", U / 10, U % 10);
  return 0;
}
#  elif defined(NEWER)
#    define CAT2(A, B) A##B
#    define CAT(A, B) CAT2(A, B)
#    ifdef SECOND
namespace n {
inline int vx = 7;
inline int vg = init();
inline int vh = init();
} // namespace n
EXPORT const int *CAT(NEWER, C)(void) { return &vc; }
EXPORT const int *CAT(NEWER, D)(void) { return &vd; }
EXPORT int *CAT(NEWER, X)(void) { return &n::vx; }
EXPORT int *CAT(NEWER, G)(void) { return &n::vg; }
EXPORT int *CAT(NEWER, H)(void) { return &n::vh; }
#    else
EXPORT const int *CAT(NEWER, C)(void) { return 0; }
EXPORT const int *CAT(NEWER, D)(void) { return 0; }
EXPORT int *CAT(NEWER, X)(void) { return 0; }
EXPORT int *CAT(NEWER, G)(void) { return 0; }
EXPORT int *CAT(NEWER, H)(void) { return 0; }
#    endif
#  else
#    include <stdio.h>
#    define IMPORT extern "C" __declspec(dllimport)
IMPORT const int *sC(void), *sD(void), *dC(void), *dD(void), *fC(void),
    *fD(void);
IMPORT int *dX(void), *dG(void), *dH(void), *gX(void), *gG(void), *gH(void);
static const char *one(const void *X, const void *S) {
  return !X ? "none" : X == S ? "one" : "split";
}
int main() {
  printf("d: vc %s vd %s, f: vc %s vd %s, inits %d\n", one(dC(), sC()),
         one(dD(), sD()), one(fC(), sC()), one(fD(), sD()), inits());
  if (dX())
    printf("foreign: vx %s vg %s %d, vh %s, inits %d\n", one(dX(), gX()),
           one(dG(), gG()), *dG(), one(dH(), gH()), inits());
  return 0;
}
#  endif
#endif
