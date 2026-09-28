// A Windows Itanium executable embeds the segment-heap manifest shipped beside
// wincrt. A DLL does not, and without the C library there is no wincrt
// start-up to need it.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=MANIFEST %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=MANIFEST-A64 %s
// MANIFEST:      "-defaultlib:clang_rt.wincrt.lib"
// MANIFEST-SAME: "-manifest:embed" "-manifestinput:{{[^"]*}}resource{{/|\\\\}}lib{{/|\\\\}}x86_64-unknown-windows-itanium{{/|\\\\}}segment_heap.manifest"
// MANIFEST-A64:  "-manifest:embed" "-manifestinput:{{[^"]*}}resource{{/|\\\\}}lib{{/|\\\\}}aarch64-unknown-windows-itanium{{/|\\\\}}segment_heap.manifest"
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -shared \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-MANIFEST %s \
// RUN:       --implicit-check-not='"-manifest'
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -nolibc \
// RUN:     -resource-dir=%t/resource 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-MANIFEST %s \
// RUN:       --implicit-check-not='"-manifest'
// NO-MANIFEST: lld-link{{(.exe)?}}"

//--- resource/lib/x86_64-unknown-windows-itanium/clang_rt.wincrt.lib
//--- resource/lib/x86_64-unknown-windows-itanium/segment_heap.manifest
//--- resource/lib/aarch64-unknown-windows-itanium/clang_rt.wincrt.lib
//--- resource/lib/aarch64-unknown-windows-itanium/segment_heap.manifest
