//===-- Correctly rounded cbrt for double precision ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MATH_CBRT_H
#define LLVM_LIBC_SRC___SUPPORT_MATH_CBRT_H

#include "src/__support/FPUtil/FEnvImpl.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/FPUtil/multiply_add.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h" // LIBC_UNLIKELY

namespace LIBC_NAMESPACE_DECL {

namespace math {

// Lookup table for table-based cbrt: 192 entries (64 per exp%3 group).
// Each entry stores {1/x0, cbrt(x0)} as uint64_t bit patterns.
// Index: it * 64 + (mantissa >> 46), where it = exponent % 3.
// x0 is the left endpoint of the sub-interval containing |zz|.
//
// For exp%3=0: |zz| in [1, 2), 64 sub-intervals of width 1/64
// For exp%3=1: |zz| in [2, 4), 64 sub-intervals of width 2/64
// For exp%3=2: |zz| in [4, 8), 64 sub-intervals of width 4/64
//
// In all cases, t = |zz|/x0 - 1 lies in [0, 1/64).
LIBC_INLINE_VAR constexpr uint64_t CBRT_LOOKUP[192][2] = {
    // exp%3 = 0, |zz| in [1, 2)
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x3fef81f81f81f820), UINT64_C(0x3ff01539221d4c97)},
    {UINT64_C(0x3fef07c1f07c1f08), UINT64_C(0x3ff02a3ad2ef6f48)},
    {UINT64_C(0x3fee9131abf0b767), UINT64_C(0x3ff03f06771a2e33)},
    {UINT64_C(0x3fee1e1e1e1e1e1e), UINT64_C(0x3ff0539d6521256f)},
    {UINT64_C(0x3fedae6076b981db), UINT64_C(0x3ff06800e629d671)},
    {UINT64_C(0x3fed41d41d41d41d), UINT64_C(0x3ff07c3236b0a73a)},
    {UINT64_C(0x3fecd85689039b0b), UINT64_C(0x3ff090328731deb3)},
    {UINT64_C(0x3fec71c71c71c71c), UINT64_C(0x3ff0a402fcc79298)},
    {UINT64_C(0x3fec0e070381c0e0), UINT64_C(0x3ff0b7a4b1bd64ac)},
    {UINT64_C(0x3febacf914c1bad0), UINT64_C(0x3ff0cb18b61ad8cf)},
    {UINT64_C(0x3feb4e81b4e81b4f), UINT64_C(0x3ff0de601024fb88)},
    {UINT64_C(0x3feaf286bca1af28), UINT64_C(0x3ff0f17bbcd80046)},
    {UINT64_C(0x3fea98ef606a63be), UINT64_C(0x3ff1046cb0597001)},
    {UINT64_C(0x3fea41a41a41a41a), UINT64_C(0x3ff11733d66373bd)},
    {UINT64_C(0x3fe9ec8e951033d9), UINT64_C(0x3ff129d212a9ba9c)},
    {UINT64_C(0x3fe999999999999a), UINT64_C(0x3ff13c484138704f)},
    {UINT64_C(0x3fe948b0fcd6e9e0), UINT64_C(0x3ff14e9736cdaf39)},
    {UINT64_C(0x3fe8f9c18f9c18fa), UINT64_C(0x3ff160bfc12dd091)},
    {UINT64_C(0x3fe8acb90f6bf3aa), UINT64_C(0x3ff172c2a772f508)},
    {UINT64_C(0x3fe8618618618618), UINT64_C(0x3ff184a0aa58191f)},
    {UINT64_C(0x3fe8181818181818), UINT64_C(0x3ff1965a848001d3)},
    {UINT64_C(0x3fe7d05f417d05f4), UINT64_C(0x3ff1a7f0eab8483d)},
    {UINT64_C(0x3fe78a4c8178a4c8), UINT64_C(0x3ff1b9648c38c55d)},
    {UINT64_C(0x3fe745d1745d1746), UINT64_C(0x3ff1cab612df9a45)},
    {UINT64_C(0x3fe702e05c0b8170), UINT64_C(0x3ff1dbe6236a0c45)},
    {UINT64_C(0x3fe6c16c16c16c17), UINT64_C(0x3ff1ecf55daa68a5)},
    {UINT64_C(0x3fe6816816816817), UINT64_C(0x3ff1fde45cbb1f9f)},
    {UINT64_C(0x3fe642c8590b2164), UINT64_C(0x3ff20eb3b72f42d5)},
    {UINT64_C(0x3fe6058160581606), UINT64_C(0x3ff21f63ff409043)},
    {UINT64_C(0x3fe5c9882b931057), UINT64_C(0x3ff22ff5c2fb2fd0)},
    {UINT64_C(0x3fe58ed2308158ed), UINT64_C(0x3ff240698c6746e5)},
    {UINT64_C(0x3fe5555555555555), UINT64_C(0x3ff250bfe1b082f5)},
    {UINT64_C(0x3fe51d07eae2f815), UINT64_C(0x3ff260f9454bb99c)},
    {UINT64_C(0x3fe4e5e0a72f0539), UINT64_C(0x3ff27116361abaea)},
    {UINT64_C(0x3fe4afd6a052bf5b), UINT64_C(0x3ff281172f8e7073)},
    {UINT64_C(0x3fe47ae147ae147b), UINT64_C(0x3ff290fca9c761f8)},
    {UINT64_C(0x3fe446f86562d9fb), UINT64_C(0x3ff2a0c719b4b6d1)},
    {UINT64_C(0x3fe4141414141414), UINT64_C(0x3ff2b076f131c9d7)},
    {UINT64_C(0x3fe3e22cbce4a902), UINT64_C(0x3ff2c00c9f2263ec)},
    {UINT64_C(0x3fe3b13b13b13b14), UINT64_C(0x3ff2cf888f8db02f)},
    {UINT64_C(0x3fe3813813813814), UINT64_C(0x3ff2deeb2bb7fb79)},
    {UINT64_C(0x3fe3521cfb2b78c1), UINT64_C(0x3ff2ee34da3b4fe3)},
    {UINT64_C(0x3fe323e34a2b10bf), UINT64_C(0x3ff2fd65ff1efbbc)},
    {UINT64_C(0x3fe2f684bda12f68), UINT64_C(0x3ff30c7efbee12ad)},
    {UINT64_C(0x3fe2c9fb4d812ca0), UINT64_C(0x3ff31b802fccf6a3)},
    {UINT64_C(0x3fe29e4129e4129e), UINT64_C(0x3ff32a69f78df567)},
    {UINT64_C(0x3fe27350b8812735), UINT64_C(0x3ff3393cadc50709)},
    {UINT64_C(0x3fe2492492492492), UINT64_C(0x3ff347f8aadab855)},
    {UINT64_C(0x3fe21fb78121fb78), UINT64_C(0x3ff3569e451e4c2b)},
    {UINT64_C(0x3fe1f7047dc11f70), UINT64_C(0x3ff3652dd0d71db1)},
    {UINT64_C(0x3fe1cf06ada2811d), UINT64_C(0x3ff373a7a0554cdf)},
    {UINT64_C(0x3fe1a7b9611a7b96), UINT64_C(0x3ff3820c0401be51)},
    {UINT64_C(0x3fe1811811811812), UINT64_C(0x3ff3905b4a6d76ce)},
    {UINT64_C(0x3fe15b1e5f75270d), UINT64_C(0x3ff39e95c0605a65)},
    {UINT64_C(0x3fe135c81135c811), UINT64_C(0x3ff3acbbb0e756b7)},
    {UINT64_C(0x3fe1111111111111), UINT64_C(0x3ff3bacd6561ff5d)},
    {UINT64_C(0x3fe0ecf56be69c90), UINT64_C(0x3ff3c8cb258fa341)},
    {UINT64_C(0x3fe0c9714fbcda3b), UINT64_C(0x3ff3d6b5379be10c)},
    {UINT64_C(0x3fe0a6810a6810a7), UINT64_C(0x3ff3e48be02ac0cf)},
    {UINT64_C(0x3fe0842108421084), UINT64_C(0x3ff3f24f62645865)},
    {UINT64_C(0x3fe0624dd2f1a9fc), UINT64_C(0x3ff4000000000000)},
    {UINT64_C(0x3fe0410410410410), UINT64_C(0x3ff40d9df94f1be1)},
    {UINT64_C(0x3fe0204081020408), UINT64_C(0x3ff41b298d47800e)},
    // exp%3 = 1, |zz| in [2, 4)
    {UINT64_C(0x3fe0000000000000), UINT64_C(0x3ff428a2f98d728b)},
    {UINT64_C(0x3fdf81f81f81f820), UINT64_C(0x3ff443604b34d9b3)},
    {UINT64_C(0x3fdf07c1f07c1f08), UINT64_C(0x3ff45dd7c26e54bb)},
    {UINT64_C(0x3fde9131abf0b767), UINT64_C(0x3ff4780b20906571)},
    {UINT64_C(0x3fde1e1e1e1e1e1e), UINT64_C(0x3ff491fc152578cb)},
    {UINT64_C(0x3fddae6076b981db), UINT64_C(0x3ff4abac3ee06707)},
    {UINT64_C(0x3fdd41d41d41d41d), UINT64_C(0x3ff4c51d2c807e59)},
    {UINT64_C(0x3fdcd85689039b0b), UINT64_C(0x3ff4de505da66b8d)},
    {UINT64_C(0x3fdc71c71c71c71c), UINT64_C(0x3ff4f747439b348b)},
    {UINT64_C(0x3fdc0e070381c0e0), UINT64_C(0x3ff51003420a5c07)},
    {UINT64_C(0x3fdbacf914c1bad0), UINT64_C(0x3ff52885afb02c85)},
    {UINT64_C(0x3fdb4e81b4e81b4f), UINT64_C(0x3ff540cfd6fd11c1)},
    {UINT64_C(0x3fdaf286bca1af28), UINT64_C(0x3ff558e2f6aed36c)},
    {UINT64_C(0x3fda98ef606a63be), UINT64_C(0x3ff570c04260716c)},
    {UINT64_C(0x3fda41a41a41a41a), UINT64_C(0x3ff58868e3115188)},
    {UINT64_C(0x3fd9ec8e951033d9), UINT64_C(0x3ff59fddf7a45f38)},
    {UINT64_C(0x3fd999999999999a), UINT64_C(0x3ff5b7209557b0ed)},
    {UINT64_C(0x3fd948b0fcd6e9e0), UINT64_C(0x3ff5ce31c83539df)},
    {UINT64_C(0x3fd8f9c18f9c18fa), UINT64_C(0x3ff5e512937d045f)},
    {UINT64_C(0x3fd8acb90f6bf3aa), UINT64_C(0x3ff5fbc3f20966a5)},
    {UINT64_C(0x3fd8618618618618), UINT64_C(0x3ff61246d6ad9aed)},
    {UINT64_C(0x3fd8181818181818), UINT64_C(0x3ff6289c2c8f1b70)},
    {UINT64_C(0x3fd7d05f417d05f4), UINT64_C(0x3ff63ec4d77a1b30)},
    {UINT64_C(0x3fd78a4c8178a4c8), UINT64_C(0x3ff654c1b4316dd0)},
    {UINT64_C(0x3fd745d1745d1746), UINT64_C(0x3ff66a9398ba2a39)},
    {UINT64_C(0x3fd702e05c0b8170), UINT64_C(0x3ff6803b54a34e44)},
    {UINT64_C(0x3fd6c16c16c16c17), UINT64_C(0x3ff695b9b149a438)},
    {UINT64_C(0x3fd6816816816817), UINT64_C(0x3ff6ab0f72182659)},
    {UINT64_C(0x3fd642c8590b2164), UINT64_C(0x3ff6c03d54c51818)},
    {UINT64_C(0x3fd6058160581606), UINT64_C(0x3ff6d544118c08bc)},
    {UINT64_C(0x3fd5c9882b931057), UINT64_C(0x3ff6ea245b64ef6f)},
    {UINT64_C(0x3fd58ed2308158ed), UINT64_C(0x3ff6fedee0388d4b)},
    {UINT64_C(0x3fd5555555555555), UINT64_C(0x3ff7137449123ef7)},
    {UINT64_C(0x3fd51d07eae2f815), UINT64_C(0x3ff727e53a4f645f)},
    {UINT64_C(0x3fd4e5e0a72f0539), UINT64_C(0x3ff73c3253cc8289)},
    {UINT64_C(0x3fd4afd6a052bf5b), UINT64_C(0x3ff7505c31104114)},
    {UINT64_C(0x3fd47ae147ae147b), UINT64_C(0x3ff764636974629c)},
    {UINT64_C(0x3fd446f86562d9fb), UINT64_C(0x3ff77848904cd549)},
    {UINT64_C(0x3fd4141414141414), UINT64_C(0x3ff78c0c350cf6cb)},
    {UINT64_C(0x3fd3e22cbce4a902), UINT64_C(0x3ff79faee36b2534)},
    {UINT64_C(0x3fd3b13b13b13b14), UINT64_C(0x3ff7b3312382b4b4)},
    {UINT64_C(0x3fd3813813813814), UINT64_C(0x3ff7c69379f4605b)},
    {UINT64_C(0x3fd3521cfb2b78c1), UINT64_C(0x3ff7d9d668054af7)},
    {UINT64_C(0x3fd323e34a2b10bf), UINT64_C(0x3ff7ecfa6bbca393)},
    {UINT64_C(0x3fd2f684bda12f68), UINT64_C(0x3ff8000000000000)},
    {UINT64_C(0x3fd2c9fb4d812ca0), UINT64_C(0x3ff812e79cae7eb9)},
    {UINT64_C(0x3fd29e4129e4129e), UINT64_C(0x3ff825b1b6bac03b)},
    {UINT64_C(0x3fd27350b8812735), UINT64_C(0x3ff8385ec043c71d)},
    {UINT64_C(0x3fd2492492492492), UINT64_C(0x3ff84aef28accd48)},
    {UINT64_C(0x3fd21fb78121fb78), UINT64_C(0x3ff85d635cb41b9d)},
    {UINT64_C(0x3fd1f7047dc11f70), UINT64_C(0x3ff86fbbc688f0e8)},
    {UINT64_C(0x3fd1cf06ada2811d), UINT64_C(0x3ff881f8cde083db)},
    {UINT64_C(0x3fd1a7b9611a7b96), UINT64_C(0x3ff8941ad80a2b83)},
    {UINT64_C(0x3fd1811811811812), UINT64_C(0x3ff8a6224802b8a8)},
    {UINT64_C(0x3fd15b1e5f75270d), UINT64_C(0x3ff8b80f7e870a2f)},
    {UINT64_C(0x3fd135c81135c811), UINT64_C(0x3ff8c9e2da25e5e4)},
    {UINT64_C(0x3fd1111111111111), UINT64_C(0x3ff8db9cb7511e9d)},
    {UINT64_C(0x3fd0ecf56be69c90), UINT64_C(0x3ff8ed3d706e1010)},
    {UINT64_C(0x3fd0c9714fbcda3b), UINT64_C(0x3ff8fec55de57860)},
    {UINT64_C(0x3fd0a6810a6810a7), UINT64_C(0x3ff91034d632b6e0)},
    {UINT64_C(0x3fd0842108421084), UINT64_C(0x3ff9218c2df27725)},
    {UINT64_C(0x3fd0624dd2f1a9fc), UINT64_C(0x3ff932cbb7f0cf2d)},
    {UINT64_C(0x3fd0410410410410), UINT64_C(0x3ff943f3c536d6e7)},
    {UINT64_C(0x3fd0204081020408), UINT64_C(0x3ff95504a517bf3b)},
    // exp%3 = 2, |zz| in [4, 8)
    {UINT64_C(0x3fd0000000000000), UINT64_C(0x3ff965fea53d6e3c)},
    {UINT64_C(0x3fcf81f81f81f820), UINT64_C(0x3ff987af34f8bb19)},
    {UINT64_C(0x3fcf07c1f07c1f08), UINT64_C(0x3ff9a907c24108e7)},
    {UINT64_C(0x3fce9131abf0b767), UINT64_C(0x3ff9ca0a8337b317)},
    {UINT64_C(0x3fce1e1e1e1e1e1e), UINT64_C(0x3ff9eab99791c790)},
    {UINT64_C(0x3fcdae6076b981db), UINT64_C(0x3ffa0b1709cc13d5)},
    {UINT64_C(0x3fcd41d41d41d41d), UINT64_C(0x3ffa2b24d04a7585)},
    {UINT64_C(0x3fccd85689039b0b), UINT64_C(0x3ffa4ae4ce6419ed)},
    {UINT64_C(0x3fcc71c71c71c71c), UINT64_C(0x3ffa6a58d55e307c)},
    {UINT64_C(0x3fcc0e070381c0e0), UINT64_C(0x3ffa8982a5567031)},
    {UINT64_C(0x3fcbacf914c1bad0), UINT64_C(0x3ffaa863ee1eaffd)},
    {UINT64_C(0x3fcb4e81b4e81b4f), UINT64_C(0x3ffac6fe500ab570)},
    {UINT64_C(0x3fcaf286bca1af28), UINT64_C(0x3ffae5535cb14343)},
    {UINT64_C(0x3fca98ef606a63be), UINT64_C(0x3ffb036497a15a18)},
    {UINT64_C(0x3fca41a41a41a41a), UINT64_C(0x3ffb2133770c88d5)},
    {UINT64_C(0x3fc9ec8e951033d9), UINT64_C(0x3ffb3ec164671755)},
    {UINT64_C(0x3fc999999999999a), UINT64_C(0x3ffb5c0fbcfec4d4)},
    {UINT64_C(0x3fc948b0fcd6e9e0), UINT64_C(0x3ffb791fd288c470)},
    {UINT64_C(0x3fc8f9c18f9c18fa), UINT64_C(0x3ffb95f2eba793dc)},
    {UINT64_C(0x3fc8acb90f6bf3aa), UINT64_C(0x3ffbb28a44693be4)},
    {UINT64_C(0x3fc8618618618618), UINT64_C(0x3ffbcee70ebe7ec9)},
    {UINT64_C(0x3fc8181818181818), UINT64_C(0x3ffbeb0a72eb6e31)},
    {UINT64_C(0x3fc7d05f417d05f4), UINT64_C(0x3ffc06f58ff1d8b5)},
    {UINT64_C(0x3fc78a4c8178a4c8), UINT64_C(0x3ffc22a97bf5f698)},
    {UINT64_C(0x3fc745d1745d1746), UINT64_C(0x3ffc3e27449db535)},
    {UINT64_C(0x3fc702e05c0b8170), UINT64_C(0x3ffc596fef6af983)},
    {UINT64_C(0x3fc6c16c16c16c17), UINT64_C(0x3ffc74847a112b65)},
    {UINT64_C(0x3fc6816816816817), UINT64_C(0x3ffc8f65dac655a3)},
    {UINT64_C(0x3fc642c8590b2164), UINT64_C(0x3ffcaa1500902099)},
    {UINT64_C(0x3fc6058160581606), UINT64_C(0x3ffcc492d38ce8db)},
    {UINT64_C(0x3fc5c9882b931057), UINT64_C(0x3ffcdee035392e38)},
    {UINT64_C(0x3fc58ed2308158ed), UINT64_C(0x3ffcf8fe00b19368)},
    {UINT64_C(0x3fc5555555555555), UINT64_C(0x3ffd12ed0af1a27f)},
    {UINT64_C(0x3fc51d07eae2f815), UINT64_C(0x3ffd2cae230f870b)},
    {UINT64_C(0x3fc4e5e0a72f0539), UINT64_C(0x3ffd46421274eaf3)},
    {UINT64_C(0x3fc4afd6a052bf5b), UINT64_C(0x3ffd5fa99d152090)},
    {UINT64_C(0x3fc47ae147ae147b), UINT64_C(0x3ffd78e581a0c130)},
    {UINT64_C(0x3fc446f86562d9fb), UINT64_C(0x3ffd91f679b6e505)},
    {UINT64_C(0x3fc4141414141414), UINT64_C(0x3ffdaadd3a1416c0)},
    {UINT64_C(0x3fc3e22cbce4a902), UINT64_C(0x3ffdc39a72bf2303)},
    {UINT64_C(0x3fc3b13b13b13b14), UINT64_C(0x3ffddc2ecf33e1b5)},
    {UINT64_C(0x3fc3813813813814), UINT64_C(0x3ffdf49af68c1570)},
    {UINT64_C(0x3fc3521cfb2b78c1), UINT64_C(0x3ffe0cdf8ba67b49)},
    {UINT64_C(0x3fc323e34a2b10bf), UINT64_C(0x3ffe24fd2d4c23b9)},
    {UINT64_C(0x3fc2f684bda12f68), UINT64_C(0x3ffe3cf476542bd0)},
    {UINT64_C(0x3fc2c9fb4d812ca0), UINT64_C(0x3ffe54c5fdc5ec73)},
    {UINT64_C(0x3fc29e4129e4129e), UINT64_C(0x3ffe6c7256f9b405)},
    {UINT64_C(0x3fc27350b8812735), UINT64_C(0x3ffe83fa11b81dbc)},
    {UINT64_C(0x3fc2492492492492), UINT64_C(0x3ffe9b5dba58189d)},
    {UINT64_C(0x3fc21fb78121fb78), UINT64_C(0x3ffeb29dd9dbaf25)},
    {UINT64_C(0x3fc1f7047dc11f70), UINT64_C(0x3ffec9baf60b9f80)},
    {UINT64_C(0x3fc1cf06ada2811d), UINT64_C(0x3ffee0b59191d375)},
    {UINT64_C(0x3fc1a7b9611a7b96), UINT64_C(0x3ffef78e2c12c61b)},
    {UINT64_C(0x3fc1811811811812), UINT64_C(0x3fff0e454245e4c0)},
    {UINT64_C(0x3fc15b1e5f75270d), UINT64_C(0x3fff24db4e0cf78b)},
    {UINT64_C(0x3fc135c81135c811), UINT64_C(0x3fff3b50c68a9dd3)},
    {UINT64_C(0x3fc1111111111111), UINT64_C(0x3fff51a62037e955)},
    {UINT64_C(0x3fc0ecf56be69c90), UINT64_C(0x3fff67dbccf922dd)},
    {UINT64_C(0x3fc0c9714fbcda3b), UINT64_C(0x3fff7df23c31c279)},
    {UINT64_C(0x3fc0a6810a6810a7), UINT64_C(0x3fff93e9dad7a4a7)},
    {UINT64_C(0x3fc0842108421084), UINT64_C(0x3fffa9c313858568)},
    {UINT64_C(0x3fc0624dd2f1a9fc), UINT64_C(0x3fffbf7e4e8cc9cc)},
    {UINT64_C(0x3fc0410410410410), UINT64_C(0x3fffd51bf2069fe7)},
    {UINT64_C(0x3fc0204081020408), UINT64_C(0x3fffea9c61e47cd3)},
};

// Correctly rounded cbrt using table-based initial approximation (~41 bits)
// with a single Newton-Raphson refinement for correct rounding (~80+ bits).
//
// The algorithm:
//   1. Range reduction: x = sign * 2^(3q + r) * m → cbrt(x) = sign * 2^q * cbrt(2^r * m).
//   2. Table lookup: get 1/x0 and cbrt(x0) for the sub-interval containing |zz|.
//   3. Polynomial: degree-4 Taylor series for (cbrt(1+t)-1)/t, t = |zz|/x0 - 1.
//   4. One Newton-Raphson refinement with FMA for ~80+ bits of accuracy.
//   5. Two hardcoded exceptional cases for round-to-nearest.
LIBC_INLINE constexpr double cbrt(double x) {
  using FPBits = fputil::FPBits<double>;

  FPBits x_bits(x);
  uint64_t x_u = x_bits.uintval();
  uint64_t x_abs = x_u & 0x7FFF'FFFF'FFFF'FFFF;
  uint64_t x_sign = x_u >> 63;

  unsigned e = (x_u >> 52) & 0x7FF;

  // Handle special cases: 0, inf, nan, subnormal.
  if (LIBC_UNLIKELY(((e + 1) & 0x7FF) < 2)) {
    if (e == 0x7FF || x_abs == 0)
      return static_cast<double>(x + x);

    // Subnormal: normalize by counting leading zeros.
    int nz = cpp::countl_zero(x_abs) - 11;
    uint64_t mant = (x_abs << nz) & 0x000F'FFFF'FFFF'FFFF;
    e -= static_cast<unsigned>(nz - 1);
    x_u = mant | (static_cast<uint64_t>(e) << 52) | (x_sign << 63);
  }

  uint64_t mant = x_u & 0x000F'FFFF'FFFF'FFFF;

  e += 3072;
  unsigned et = e / 3;
  unsigned it = e % 3;

  // |zz| = 2^it * 1.mantissa ∈ [1, 8), zz = sign * |zz|.
  double zz_abs =
      FPBits(mant | (static_cast<uint64_t>(0x3FF + it) << 52)).get_val();
  double zz =
      FPBits(mant | (static_cast<uint64_t>(0x3FF + it) << 52) | (x_sign << 63))
          .get_val();

  // Start the division early for the Newton step.
  double rr = 1.0 / zz;

  // Table lookup: index = it * 64 + top 6 mantissa bits.
  unsigned tab_idx = it * 64 + static_cast<unsigned>(mant >> 46);
  double rcp_x0 = FPBits(CBRT_LOOKUP[tab_idx][0]).get_val();
  double cbrt_x0 = FPBits(CBRT_LOOKUP[tab_idx][1]).get_val();

  // t = |zz|/x0 - 1 ∈ [0, 1/64), computed via FMA for accuracy.
  double t = fputil::multiply_add(zz_abs, rcp_x0, -1.0);

  // Degree-4 polynomial P(t) ≈ (cbrt(1+t) - 1) / t.
  // Taylor coefficients: 1/3, -1/9, 5/81, -10/243, 22/729.
  constexpr double PC0 = 0x1.5555555555555p-2;   //  1/3
  constexpr double PC1 = -0x1.c71c71c71c71cp-4;  // -1/9
  constexpr double PC2 = 0x1.f9add3c0ca458p-5;   //  5/81
  constexpr double PC3 = -0x1.511e8d2b3183bp-5;  // -10/243
  constexpr double PC4 = 0x1.ee7113506ac12p-6;   //  22/729

  double p = fputil::multiply_add(t, PC4, PC3);
  p = fputil::multiply_add(t, p, PC2);
  p = fputil::multiply_add(t, p, PC1);
  p = fputil::multiply_add(t, p, PC0);

  // y = cbrt_x0 * (1 + t * P(t)), with sign applied.
  double correction = t * p;
  double y = fputil::multiply_add(cbrt_x0, correction, cbrt_x0);
  uint64_t y_u = FPBits(y).uintval() | (x_sign << 63);
  y = FPBits(y_u).get_val();

  // Newton-Raphson refinement using FMA for double-double y^3.
  constexpr double ONE_THIRD = 0x1.5555555555555p-2;

  double y2 = y * y;
  double y2l = fputil::multiply_add(y, y, -y2);
  double y3 = y2 * y;
  double y3l = fputil::multiply_add(y, y2, -y3) + y * y2l;
  double h = ((y3 - zz) + y3l) * rr;
  double dy = h * (y * ONE_THIRD);
  double y1 = y - dy;
  double dy_rem = (y - y1) - dy;

#ifndef LIBC_MATH_CBRT_SKIP_ACCURATE_PASS
  // Check if we're near a rounding boundary.
  double ady = FPBits(dy_rem).abs().get_val();
  double ady0 = FPBits(ady - 0x1p-53).abs().get_val();
  double ady1 = FPBits(ady - 0x1p-52 - 0x1p-53).abs().get_val();

  if (LIBC_UNLIKELY(ady0 < 0x1p-75 || ady1 < 0x1p-75)) {
    y2 = y1 * y1;
    y2l = fputil::multiply_add(y1, y1, -y2);
    y3 = y2 * y1;
    y3l = fputil::multiply_add(y1, y2, -y3) + y1 * y2l;
    h = ((y3 - zz) + y3l) * rr;
    dy = h * (y1 * ONE_THIRD);
    y = y1 - dy;
    dy_rem = (y1 - y) - dy;
    y1 = y;
    ady = FPBits(dy_rem).abs().get_val();
    ady0 = FPBits(ady - 0x1p-53).abs().get_val();
    ady1 = FPBits(ady - 0x1p-52 - 0x1p-53).abs().get_val();

    if (LIBC_UNLIKELY(ady0 < 0x1p-98 || ady1 < 0x1p-98)) {
      double azz = FPBits(zz).abs().get_val();
      if (azz == 0x1.9b78223aa307cp+1)
        y1 = FPBits(0x3FF79D15'0E8D59BCull | (x_sign << 63)).get_val();
      if (azz == 0x1.a202bfc89ddffp+2)
        y1 = FPBits(0x3FFDE87A'A837820Full | (x_sign << 63)).get_val();
    }
  }
#endif // LIBC_MATH_CBRT_SKIP_ACCURATE_PASS

  // Apply the exponent: multiply by 2^(et - 342 - 1023).
  FPBits y1_bits(y1);
  uint64_t y1_u = y1_bits.uintval();
  y1_u += static_cast<uint64_t>(et - 342 - 1023) << 52;

#ifndef LIBC_MATH_CBRT_SKIP_ACCURATE_PASS
  int64_t m0 = static_cast<int64_t>(y1_u << 30);
  int64_t m1 = m0 >> 63;
  if (LIBC_UNLIKELY(static_cast<uint64_t>(m0 ^ m1) <= (1ull << 30))) {
    FPBits y1r_bits(y1);
    uint64_t y1r_u = (y1r_bits.uintval() + (1ull << 15)) &
                     0xFFFF'FFFF'FFFF'0000ull;
    double y1r = FPBits(y1r_u).get_val();
    if (FPBits((y1r - y1) - dy_rem).abs().get_val() < 0x1p-60 ||
        FPBits(zz).abs().get_val() == 1.0) {
      y1_u = (y1_u + (1ull << 15)) & 0xFFFF'FFFF'FFFF'0000ull;
      fputil::clear_except_if_required(FE_INEXACT);
    }
  }
#endif // LIBC_MATH_CBRT_SKIP_ACCURATE_PASS

  return FPBits(y1_u).get_val();
}

} // namespace math

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_MATH_CBRT_H
