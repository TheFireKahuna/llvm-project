# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %s -o %t.obj
# RUN: llvm-readobj --symbols %t.obj | FileCheck %s
# RUN: not llvm-mc -filetype=obj -triple=x86_64-windows-msvc --defsym=ERR=1 %s \
# RUN:   -o /dev/null 2>&1 | FileCheck --check-prefix=ERR %s

## COFF has no symbol versions, so .symver defines the versioned name as an
## external alias of the symbol, which a linker exports under its version.
## "@@@" names the default version, as "@@" does.

        .text
        .globl foo_v1, foo_v2, foo_v3
foo_v1:
        retq
foo_v2:
        retq
foo_v3:
        retq
        .symver foo_v1, foo@V1
        .symver foo_v2, foo@V2
        .symver foo_v3, foo@@@V3

# CHECK:      Name: foo@V1
# CHECK-NEXT: Value: 0
# CHECK-NEXT: Section: .text
# CHECK-NEXT: BaseType:
# CHECK-NEXT: ComplexType:
# CHECK-NEXT: StorageClass: External
# CHECK:      Name: foo@V2
# CHECK-NEXT: Value: 1
# CHECK:      Name: foo@@V3
# CHECK-NEXT: Value: 2

.ifdef ERR
# ERR: [[#@LINE+1]]:28: error: expected a '@' in the name
        .symver foo_v1, foo
# ERR: [[#@LINE+1]]:33: error: 'remove' is not supported for COFF
        .symver foo_v1, foo@V1, remove
.endif
