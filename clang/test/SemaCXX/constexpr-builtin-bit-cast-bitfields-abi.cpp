// RUN: %clang_cc1 -std=c++2a -fsyntax-only -triple x86_64-linux-gnu -verify=itanium %s
// RUN: %clang_cc1 -std=c++2a -fsyntax-only -triple x86_64-linux-gnu -fexperimental-new-constant-interpreter -verify=itanium %s
// RUN: %clang_cc1 -std=c++2a -fsyntax-only -triple x86_64-pc-windows-msvc -verify=ms %s
// RUN: %clang_cc1 -std=c++2a -fsyntax-only -triple x86_64-pc-windows-msvc -fexperimental-new-constant-interpreter -verify=ms %s

/// Neither constant evaluator computes bit-field placement itself: both take it
/// from ASTContext::getASTRecordLayout(), which dispatches to the ABI-specific
/// record layout builder (ItaniumRecordLayoutBuilder vs
/// MicrosoftRecordLayoutBuilder) and is the same layout CodeGen uses. So
/// __builtin_bit_cast automatically follows whichever C++ ABI is in effect.
///
/// For a struct whose bit-fields have different underlying types, the two ABIs
/// disagree: the Itanium ABI packs both bit-fields into a single 4-byte
/// allocation unit, while the Microsoft ABI gives 'b' its own unit -- so both
/// the size and the bit positions differ. Both constant evaluators (current and
/// bytecode) must agree with their ABI.

template <class To, class From>
constexpr To bit_cast(const From &from) {
  return __builtin_bit_cast(To, from);
}

struct S {
  unsigned char a : 4;
  unsigned int  b : 4;
};

/// The two ABIs disagree about the size, which is the layout difference that
/// bit_cast below depends on. Each assertion holds under exactly one ABI.
static_assert(sizeof(S) == 4); // ms-error {{static assertion failed due to requirement 'sizeof(S) == 4'}} \
                               // ms-note {{expression evaluates to '8 == 4'}}
static_assert(sizeof(S) == 8); // itanium-error {{static assertion failed due to requirement 'sizeof(S) == 8'}} \
                               // itanium-note {{expression evaluates to '4 == 8'}}

#ifdef _WIN32
/// Microsoft: 'a' occupies bits 0-3 of byte 0; 'b' starts a new allocation unit
/// at byte 4 (bit 32). The bits in between are padding.
constexpr S s = bit_cast<S>(0x0000000B0000000AULL);
#else
/// Itanium: 'a' is in bits 0-3 and 'b' is packed right after it in bits 4-7.
constexpr S s = bit_cast<S>(0xBAu);
#endif
static_assert(s.a == 0xA);
static_assert(s.b == 0xB);

/// A value whose only set bit is bit 4 decodes to b == 1 under Itanium (bit 4
/// belongs to 'b') but b == 0 under Microsoft (bit 4 is padding; 'b' lives at
/// bit 32).
#ifdef _WIN32
constexpr S t = bit_cast<S>(0x0000000000000010ULL);
static_assert(t.b == 0);
#else
constexpr S t = bit_cast<S>(0x00000010u);
static_assert(t.b == 1);
#endif
