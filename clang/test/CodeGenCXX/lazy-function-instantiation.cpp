// RUN: %clang_cc1 -triple x86_64-linux-gnu -std=c++20 -emit-llvm -o - %s \
// RUN:   -flazy-function-instantiation | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-linux-gnu -std=c++20 -emit-llvm -o - %s \
// RUN:   -flazy-function-instantiation -O2 -disable-llvm-passes \
// RUN:   -mconstructor-aliases | FileCheck %s
// RUN: not %clang_cc1 -triple x86_64-linux-gnu -std=c++20 -emit-llvm -o - %s \
// RUN:   2>&1 | FileCheck --check-prefix=EAGER %s

// Used from emitted code: instantiated and emitted.
template <class T> int used(T t) { return t + 1; }

// Only used from an inline function that is never emitted: with
// -flazy-function-instantiation it is never instantiated, so the ill-formed
// body isn't diagnosed.
template <class T> int unused(T t) { return t.nope(); }
// EAGER: error: member reference base type 'int' is not a structure or union
inline int never_emitted(int x) { return unused(x); }

// Instantiated on demand by the constant evaluator.
template <class T> constexpr int twice(T t) { return t * 2; }
static_assert(twice(21) == 42);
constexpr int (*twice_ptr)(int) = twice<int>;
int call_through_constant(int x) { return twice_ptr(x); }

// Instantiated on demand by CodeGen, transitively.
template <class T> int leaf(T t) { return t; }
template <class T> int middle(T t) { return leaf(t) + 1; }

// Reached through a friend redeclaration.
struct S {
  template <class T> friend int fr(S, T) { return 3; }
};

// Members with 'used' are emitted even if nothing references them.
template <class T> struct WithUsed {
  __attribute__((used)) int f() { return 4; }
};
template struct WithUsed<long>;
WithUsed<int> with_used;

// Virtual functions are needed by the vtable.
template <class T> struct V {
  virtual int vf() { return 5; }
};
V<int> v;

int f(int x) { return used(x) + middle(x) + fr(S{}, 1); }

// CHECK-DAG: define {{.*}} @_Z4usedIiEiT_(
// CHECK-DAG: define {{.*}} @_Z5twiceIiEiT_(
// CHECK-DAG: define {{.*}} @_Z6middleIiEiT_(
// CHECK-DAG: define {{.*}} @_Z4leafIiEiT_(
// CHECK-DAG: define {{.*}} @_Z2frIiEi1ST_(
// CHECK-DAG: define {{.*}} @_ZN8WithUsedIiE1fEv(
// CHECK-DAG: define {{.*}} @_ZN1VIiE2vfEv(
// CHECK-DAG: define {{.*}} @_ZN4BaseIiED2Ev(
// CHECK-NOT: @_Z6unusedIiEiT_

// At -O1 and higher, ~Derived is emitted as a call to Base<int>::~Base; the
// reference only appears when CodeGen applies that replacement.
void g();
template <class T> struct Base { ~Base() { g(); } };
struct Derived : Base<int> { ~Derived() {} };
void use_derived() { Derived d; }
