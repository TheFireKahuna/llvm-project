// REQUIRES: x86
// RUN: split-file %s %t.dir && cd %t.dir

// Build the pieces.
// RUN: llvm-mc -filetype=obj -triple=x86_64-windows main.s            -o main.obj
// RUN: llvm-mc -filetype=obj -triple=x86_64-windows dispatcher.s      -o dispatcher.obj
// RUN: llvm-mc -filetype=obj -triple=x86_64-windows bad.s             -o bad-dispatcher.obj
// RUN: llvm-mc -filetype=obj -triple=x86_64-windows member-foo-a.s    -o member-foo-a.obj
// RUN: llvm-mc -filetype=obj -triple=x86_64-windows member-foo-b.s    -o member-foo-b.obj
// RUN: llvm-mc -filetype=obj -triple=x86_64-windows member-unrelated.s -o member-unrelated.obj
// RUN: llvm-lib -out:members.lib member-foo-a.obj member-foo-b.obj member-unrelated.obj

// /includeglob: in a .drectve directive must pull every archive member whose
// symbol matches the glob, even when the dispatcher TU is the only outward
// reference into the archive (no /include: anywhere on the command line for
// the matched symbols, no other source-level reference).
//
// RUN: lld-link /out:t.exe /entry:main main.obj dispatcher.obj members.lib /verbose >& t.log
// RUN: FileCheck --check-prefix=PULLED %s < t.log
//
// PULLED-DAG: members.lib(member-foo-a.obj) for foo_a
// PULLED-DAG: members.lib(member-foo-b.obj) for foo_b
// PULLED-NOT: for bar

// Invalid glob pattern surfaces the same error as the command-line form.
//
// RUN: not lld-link /out:err.exe /entry:main main.obj bad-dispatcher.obj members.lib 2>&1 \
// RUN:   | FileCheck --check-prefix=ERR %s
//
// ERR: /includeglob:

#--- main.s
        .text
        .globl main
main:
        retq

#--- dispatcher.s
        .section .drectve, "yn"
        .ascii " /includeglob:foo_*"

#--- bad.s
        .section .drectve, "yn"
        .ascii " /includeglob:["

#--- member-foo-a.s
        .text
        .globl foo_a
foo_a:
        retq

#--- member-foo-b.s
        .text
        .globl foo_b
foo_b:
        retq

#--- member-unrelated.s
        .text
        .globl bar
bar:
        retq
