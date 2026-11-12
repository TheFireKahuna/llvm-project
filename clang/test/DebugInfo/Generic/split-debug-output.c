// REQUIRES: x86-registered-target
// RUN: %clang_cc1 -triple x86_64-unknown-linux -debug-info-kind=limited -dwarf-version=4 -split-dwarf-file foo.dwo -split-dwarf-output %t -emit-obj -o - %s | llvm-dwarfdump -debug-info - | FileCheck --check-prefix=DWARFv4 %s
// RUN: llvm-dwarfdump -debug-info %t | FileCheck --check-prefix=DWARFv4 %s

// RUN: %clang_cc1 -triple x86_64-unknown-linux -debug-info-kind=limited -dwarf-version=5 -split-dwarf-file foo.dwo -split-dwarf-output %t -emit-obj -o - %s | llvm-dwarfdump -debug-info - | FileCheck --check-prefix=DWARFv5 %s
// RUN: llvm-dwarfdump -debug-info %t | FileCheck --check-prefix=DWARFv5 %s

// The .dwo is written relative to -working-directory like the object file.
// RUN: rm -rf %t.d && mkdir %t.d
// RUN: %clang_cc1 -triple x86_64-unknown-linux -debug-info-kind=limited -dwarf-version=5 -split-dwarf-file foo.dwo -split-dwarf-output foo.dwo -emit-obj -o foo.o -working-directory %t.d %s
// RUN: llvm-dwarfdump -debug-info %t.d/foo.dwo | FileCheck --check-prefix=DWARFv5 %s

int f(void) { return 0; }

// DWARFv4: DW_AT_GNU_dwo_name ("foo.dwo")
// DWARFv5: DW_AT_dwo_name ("foo.dwo")
