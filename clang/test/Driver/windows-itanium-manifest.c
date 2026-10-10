// Every executable embeds a manifest. On Windows Itanium it is the
// segment-heap manifest shipped beside wincrt, whether or not the C library is
// linked. A DLL embeds none.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=MANIFEST %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -nolibc \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=MANIFEST %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -nostdlib \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=MANIFEST %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=MANIFEST-A64 %s
// MANIFEST:      "-manifest:embed" "-manifestinput:{{[^"]*}}resource{{/|\\\\}}lib{{/|\\\\}}x86_64-unknown-windows-itanium{{/|\\\\}}segment_heap.manifest"
// MANIFEST-A64:  "-manifest:embed" "-manifestinput:{{[^"]*}}resource{{/|\\\\}}lib{{/|\\\\}}aarch64-unknown-windows-itanium{{/|\\\\}}segment_heap.manifest"

// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s -nostdlib 2>&1 \
// RUN:   | FileCheck --check-prefix=NTPOSIX %s \
// RUN:       --implicit-check-not='"-manifestinput:'
// NTPOSIX: "-manifest:embed"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -shared \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-MANIFEST %s \
// RUN:       --implicit-check-not='"-manifest'
// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s -shared -nostdlib \
// RUN:     2>&1 \
// RUN:   | FileCheck --check-prefix=NO-MANIFEST %s \
// RUN:       --implicit-check-not='"-manifest'
// NO-MANIFEST: lld-link{{(.exe)?}}"

//--- resource/lib/x86_64-unknown-windows-itanium/clang_rt.wincrt.lib
//--- resource/lib/x86_64-unknown-windows-itanium/segment_heap.manifest
//--- resource/lib/aarch64-unknown-windows-itanium/clang_rt.wincrt.lib
//--- resource/lib/aarch64-unknown-windows-itanium/segment_heap.manifest
