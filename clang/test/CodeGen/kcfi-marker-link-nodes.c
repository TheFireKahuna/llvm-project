// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A link fact names the types a function reaches through a record by a node:
/// __kcfi_inflow_n<node>_<function> or __kcfi_param_n<node>_<function>, whose
/// types the object lists once as __kcfi_node_<node>_<type>, in ascending
/// order. The node is named by a hash of its types, so two records with the
/// same types, and one record that two functions reach the same way, share
/// it; a record reached by the inflow walk, which follows pointers, and the
/// same record reached by the parameter walk, which does not, give two nodes.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);

struct other {
  c_fn f;
};

struct big {
  a_fn a;
  b_fn b;
  struct big *next;
  struct other *o;
};

/// The same types as big holds itself.
struct twin {
  b_fn b;
  a_fn a;
};

void def_take(struct big *b) {}
void def_take2(struct twin t, d_fn d) {}
struct big *foreign_get(void);
void foreign_out(struct big **out);

void use(void) {
  struct big *b;
  foreign_get();
  foreign_out(&b);
}

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_param_n[[HELD:[0-9a-f]+]]_def_take"
// CHECK-NEXT: ".set __kcfi_param_n[[HELD]]_def_take, 0"
// CHECK-NEXT: ".weak __kcfi_node_[[HELD]]_[[T1:[0-9a-f]+]]"
// CHECK-NEXT: ".set __kcfi_node_[[HELD]]_[[T1]], {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_node_[[HELD]]_[[T2:[0-9a-f]+]]"
// CHECK-NEXT: ".set __kcfi_node_[[HELD]]_[[T2]], {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_param_[[D:[0-9a-f]+]]_def_take2"
// CHECK-NEXT: ".set __kcfi_param_[[D]]_def_take2, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_param_n[[HELD]]_def_take2"
// CHECK-NEXT: ".set __kcfi_param_n[[HELD]]_def_take2, 0"
// CHECK-NEXT: ".weak __kcfi_inflow_n[[REACHED:[0-9a-f]+]]_foreign_get"
// CHECK-NEXT: ".set __kcfi_inflow_n[[REACHED]]_foreign_get, 0"
// CHECK-NEXT: ".weak __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_inflow_n[[REACHED]]_foreign_out"
// CHECK-NEXT: ".set __kcfi_inflow_n[[REACHED]]_foreign_out, 0"
// CHECK-NOT:  __kcfi_
