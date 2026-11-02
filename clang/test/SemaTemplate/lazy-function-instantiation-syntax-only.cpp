// RUN: %clang_cc1 -std=c++20 -fsyntax-only -flazy-function-instantiation \
// RUN:   -verify %s

// Without CodeGen, -flazy-function-instantiation is ignored: nothing would
// request the instantiations, so everything is instantiated as usual.
template <class T> int f(T t) {
  return t.nope(); // expected-error {{member reference base type 'int' is not a structure or union}}
}
int g(int x) { return f(x); } // expected-note {{in instantiation of function template specialization 'f<int>' requested here}}
