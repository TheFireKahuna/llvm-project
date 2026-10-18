// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck %s

// On NT-POSIX an external extern "C" function carries the face it was
// declared in, its calling convention and the widths that face decides, as a
// contract named for the symbol with "$face": an import requires it, an export
// provides it, and the linker checks that every party agrees.

#define API __attribute__((visibility("default")))

API long imported(long);
API long exported(long x) { return imported(x); }

// A datum carries one only where its type holds something a face decides.
API extern long widths;
API extern int plain;
long use(void) { return widths + plain; }

// CHECK: !abi.requirements = !{![[IMP:[0-9]+]], ![[WID:[0-9]+]]}
// CHECK: ![[IMP]] = !{ptr @imported, ![[IMPFACE:[0-9]+]]}
// CHECK: ![[IMPFACE]] = !{!"imported$face", !"{{.*}}"}
// CHECK: ![[WID]] = !{ptr @widths, ![[WIDFACE:[0-9]+]]}
// CHECK: ![[WIDFACE]] = !{!"widths$face", !"{{.*}}"}
// CHECK: !{!"/abiprovide:{{[A-Za-z0-9_-]+}}"}
