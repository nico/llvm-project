// RUN: %clang_cc1 -verify=ref,both -std=c++2a -fsyntax-only -triple x86_64-linux-gnu %s
// RUN: %clang_cc1 -verify=ref,both -std=c++2a -fsyntax-only -triple aarch64_be-linux-gnu %s
// RUN: %clang_cc1 -verify=expected,both -std=c++2a -fsyntax-only -triple x86_64-linux-gnu -fexperimental-new-constant-interpreter %s
// RUN: %clang_cc1 -verify=expected,both -std=c++2a -fsyntax-only -triple aarch64_be-linux-gnu -fexperimental-new-constant-interpreter %s

/// __builtin_bit_cast involving bit-fields. The cases here are mostly
/// well-defined in both constant evaluators; the Indeterminate namespace also
/// covers reads of uninitialized bit-field padding, where the two evaluators
/// differ. More exhaustive coverage is in
/// test/AST/ByteCode/builtin-bit-cast-bitfields.cpp.

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define LITTLE_END 1
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define LITTLE_END 0
#else
#  error "huh?"
#endif

typedef unsigned __INT16_TYPE__ uint16_t;
typedef unsigned __INT32_TYPE__ uint32_t;

template <class To, class From>
constexpr To bit_cast(const From &from) {
  static_assert(sizeof(To) == sizeof(From));
  return __builtin_bit_cast(To, from);
}

template <class Intermediate, class Init>
constexpr Init round_trip(const Init &init) {
  return bit_cast<Init>(bit_cast<Intermediate>(init));
}

namespace SplitInteger {
  struct BitFields {
    unsigned a : 2;
    unsigned b : 30;
  };
  constexpr unsigned A = __builtin_bit_cast(unsigned, BitFields{3, 16});
  static_assert(A == (LITTLE_END ? 67 : 3221225488));

  struct S {
    unsigned a : 2;
    unsigned b : 28;
    unsigned c : 2;
  };
  constexpr S s = __builtin_bit_cast(S, 0xFFFFFFFF);
  static_assert(s.a == 3);
  static_assert(s.b == 268435455);
  static_assert(s.c == 3);
}

namespace TwoShorts {
  struct B {
    unsigned short s0 : 8;
    unsigned short s1 : 8;
  };
  constexpr struct { unsigned short b1; } T = {0xc0ff};
  constexpr B MB = __builtin_bit_cast(B, T);
  static_assert(MB.s0 == (LITTLE_END ? 0xff : 0xc0));
  static_assert(MB.s1 == (LITTLE_END ? 0xc0 : 0xff));
}

namespace SignExtension {
  struct R {
    unsigned int r : 31;
    unsigned int : 0;
    unsigned int : 32;
    constexpr bool operator==(R const &other) const { return r == other.r; }
  };
  struct T {
    signed long long t : 31;
    constexpr bool operator==(T const &other) const { return t == other.t; }
  };
  constexpr R r{0x4ac0ffee};
  constexpr T t = bit_cast<T>(r);
  static_assert(t.t == ((0xFFFFFFFF8 << 28) | 0x4ac0ffee)); // sign extension
  static_assert(round_trip<T>(r) == r);
  static_assert(round_trip<R>(t) == t);
}

namespace Nibbles {
  struct S {
    unsigned char x : 4;
    unsigned char y : 4;
    constexpr bool operator==(S const &other) const {
      return x == other.x && y == other.y;
    }
  };
  constexpr S s{0xa, 0xb};
  static_assert(__builtin_bit_cast(unsigned char, s) == (LITTLE_END ? 0xba : 0xab));
  static_assert(round_trip<unsigned char>(s) == s);

  /// Decompose one byte into two 4-bit fields.
  constexpr struct { unsigned char b0; } T = {0xee};
  constexpr S MB = __builtin_bit_cast(S, T);
  static_assert(MB.x == 0xe);
  static_assert(MB.y == 0xe);
}

namespace Nested {
  struct J {
    struct { uint16_t k : 12; } K;
    struct { uint16_t l : 4; } L;
  };
  static_assert(sizeof(J) == 4);
  constexpr J j = bit_cast<J>(0x8c0ffee5u);
  static_assert(j.K.k == (LITTLE_END ? 0xee5 : 0x8c0));
  static_assert(j.L.l == 0xf);
}

namespace Enums {
  constexpr int pad = LITTLE_END ? 6 : 0;
  struct X {
    char : pad;
    enum class direction : char { left, right, up, down } direction : 2;
  };
  constexpr X x = {X::direction::down};
  static_assert(
      bit_cast<X>((unsigned char)0x40).direction == X::direction::right);
}

namespace Indeterminate {
  /// A bit-field only covers some of the bits of its allocation unit; the
  /// remaining bits are padding. The current evaluator tracks those padding
  /// bits as uninitialized, so reading them yields an indeterminate value. The
  /// bytecode interpreter zero-initializes object storage and therefore accepts
  /// some of these (see test/AST/ByteCode/builtin-bit-cast-bitfields.cpp).
  struct S2 { unsigned char a : 2; };

  /// An 'unsigned char' may hold an indeterminate value, but an indeterminate
  /// value can't initialize a constexpr variable.
  constexpr unsigned char B = // ref-error {{must be initialized by a constant expression}} \
                              // ref-note {{subobject of type 'const unsigned char' is not initialized}}
      __builtin_bit_cast(unsigned char, S2{3});

  /// A plain enum (not std::byte) can't hold an indeterminate value at all.
  enum byte : unsigned char {};
  /// The two evaluators format the invalid type's name slightly differently.
  constexpr byte C = __builtin_bit_cast(byte, S2{3}); // both-error {{must be initialized by a constant expression}} \
                                                      // ref-note {{indeterminate value can only initialize an object of type 'unsigned char' or 'std::byte'; 'Indeterminate::byte' is invalid}} \
                                                      // expected-note {{indeterminate value can only initialize an object of type 'unsigned char' or 'std::byte'; 'byte' is invalid}}

  struct S3 { unsigned a : 13; unsigned : 17; unsigned b : 2; };
  struct D { unsigned a; };
  constexpr D d = __builtin_bit_cast(D, S3{12, 3}); // both-error {{must be initialized by a constant expression}} \
                                                    // both-note {{indeterminate value can only initialize an object of type 'unsigned char' or 'std::byte'; 'unsigned int' is invalid}}
}
