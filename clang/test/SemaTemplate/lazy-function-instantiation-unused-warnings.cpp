// RUN: %clang_cc1 -triple x86_64-linux-gnu -std=c++20 -emit-llvm -o /dev/null \
// RUN:   -flazy-function-instantiation -Wunused-function -verify %s
// RUN: %clang_cc1 -triple x86_64-linux-gnu -std=c++20 -emit-llvm -o /dev/null \
// RUN:   -Wunused-function -verify %s
// expected-no-diagnostics

// 'helper' is only used from an instantiation that lazy mode would skip.
// Rather than warning that it's unused, -flazy-function-instantiation falls
// back to instantiating everything when such a warning could fire.
static int helper() { return 1; }
template <class T> int uses_helper(T) { return helper(); }
inline int never_emitted() { return uses_helper(0); }
