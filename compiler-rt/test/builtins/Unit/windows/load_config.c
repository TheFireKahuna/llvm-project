// RUN: %clang_wincrt %s -o %t.exe
// RUN: llvm-readobj --coff-load-config %t.exe | FileCheck %s
// RUN: llvm-readobj --sections %t.exe | FileCheck %s --check-prefix=SECTIONS --implicit-check-not=.CRT

// Every image gets a load configuration that names its security cookie and
// leaves the fields of absent features zero. The .CRT tables are merged into
// .rdata.

int main(void) { return 0; }

// CHECK:      LoadConfig [
// CHECK:        SecurityCookie: 0x{{[0-9A-F]*[1-9A-F][0-9A-F]*}}
// CHECK:        EnclaveConfigurationPointer: 0x0
// CHECK-NEXT:   VolatileMetadataPointer: 0x0

// SECTIONS: Name: .rdata
