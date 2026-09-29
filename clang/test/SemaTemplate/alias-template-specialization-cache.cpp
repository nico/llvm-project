// RUN: %clang_cc1 -std=c++20 -fsyntax-only -verify %s

// Sema caches the types of alias template specializations. These check the
// cases where forming the same specialization again must not use the cache.

namespace lambda {
// Each specialization of an alias template with a lambda is a distinct type.
template <class T> using A = decltype([] {});
static_assert(!__is_same(A<int>, A<int>));
}

namespace diagnostics {
// A warning from substituting into an alias template is emitted for every use,
// both where diagnostics are delayed (in declarations) and where they aren't.
struct S { typedef int T [[deprecated]]; }; // expected-note 4 {{marked deprecated here}}
template <class U> using X = typename U::T; // expected-warning 4 {{'T' is deprecated}}
X<S> a;
X<S> b;
void f() {
  (void)sizeof(X<S>); // expected-note {{in instantiation of template type alias 'X' requested here}}
  (void)sizeof(X<S>); // expected-note {{in instantiation of template type alias 'X' requested here}}
}
}

namespace access {
// Access checks in the pattern of an alias template at namespace scope depend
// on where the alias template is used.
class C { typedef int P; friend struct F; friend void f(); }; // expected-note 2 {{implicitly declared private here}}
template <class T> using X = typename T::P; // expected-error 2 {{'P' is a private member of 'access::C'}}
struct F { X<C> m; };
X<C> g;
void f() { (void)sizeof(X<C>); }
void h() { (void)sizeof(X<C>); } // expected-note {{in instantiation of template type alias 'X' requested here}}
}
