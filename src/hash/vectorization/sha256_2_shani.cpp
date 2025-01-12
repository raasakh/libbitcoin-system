// Based on:
// sha256-x86.c - Intel SHA extensions using C intrinsics
// Written and place in public domain by Jeffrey Walton
// Based on code from Intel, and by Sean Gulley for the miTLS project.

#include <iterator>
#include <stdint.h>
#include <bitcoin/system/define.hpp>
#include <bitcoin/system/math/math.hpp>
#include <bitcoin/system/endian/endian.hpp>

namespace libbitcoin {
namespace system {
namespace sha256 {

#if defined (DISABLED)

#if !defined(HAVE_XCPU)

void hash_shani(state&, const block1&) NOEXCEPT
{
    BC_ASSERT_MSG(false, "hash_shani undefined");
}

#else

// See sse41 for defines.
using namespace i128;

#ifndef VISUAL

alignas(xint128_t) constexpr uint8_t mask[sizeof(xint128_t)]
{
    0x03, 0x02, 0x01, 0x00, // 0x00010203ul
    0x07, 0x06, 0x05, 0x04, // 0x04050607ul
    0x0b, 0x0a, 0x09, 0x08, // 0x08090a0bul
    0x0f, 0x0e, 0x0d, 0x0c  // 0x0c0d0e0ful
};

// Half of little endian IV.
alignas(xint128_t) constexpr uint8_t initial0[sizeof(xint128_t)]
{
    0x8c, 0x68, 0x05, 0x9b, // 0x9b05688cul [5]
    0x7f, 0x52, 0x0e, 0x51, // 0x510e527ful [4]
    0x85, 0xae, 0x67, 0xbb, // 0xbb67ae85ul [1]
    0x67, 0xe6, 0x09, 0x6a  // 0x6a09e667ul [0]
};

// Half of little endian IV.
alignas(xint128_t) constexpr uint8_t initial1[sizeof(xint128_t)]
{
    0x19, 0xcd, 0xe0, 0x5b, // 0x5be0cd19ul [7]
    0xab, 0xd9, 0x83, 0x1f, // 0x1f83d9abul [6]
    0x3a, 0xf5, 0x4f, 0xa5, // 0xa54ff53aul [3]
    0x72, 0xf3, 0x6e, 0x3c  // 0x3c6ef372ul [2]
};

// load/store i128
// ----------------------------------------------------------------------------

// Loading is just an array_cast into the buffer.

// Aligned only, do not use with unaligned values.
xint128_t load32x4a(const uint8_t& bytes) NOEXCEPT
{
    return _mm_load_si128(pointer_cast<const xint128_t>(&bytes));
}

xint128_t load32x4u(const uint32_t& bytes) NOEXCEPT
{
    return _mm_loadu_si128(pointer_cast<const xint128_t>(&bytes));
}
void store32x4u(uint8_t& bytes, xint128_t value) NOEXCEPT
{
    _mm_storeu_si128(pointer_cast<xint128_t>(&bytes), value);
}

// Aligned but for public data?
xint128_t load(const uint8_t& data) NOEXCEPT
{
    static const auto flipper = load32x4a(mask[0]);
    return i128::shuffle(load32x4a(data), flipper);
}

// Aligned but for public data?
void store(uint8_t& out, xint128_t value) NOEXCEPT
{
    static const auto flipper = load32x4a(mask[0]);
    store32x4u(out, i128::shuffle(value, flipper));
}

// sha256
// ----------------------------------------------------------------------------
// intel.com/content/www/us/en/developer/articles/technical/intel-sha-extensions.html
// intel.com/content/dam/develop/external/us/en/documents/intel-sha-extensions-white-paper.pdf

// _mm_sha256rnds2_epu32 is power of sha-ni, round reduction to 4 lane native.
// But this needs to be applied to preparation as well, to retain that model.
// Otherwise the round dispath must be modified to use the circular var queue.
// And this changes the size of buffer_t (to words_t).
// _mm_sha1rnds4_epu32 is provided for sha160. This would optimize only script
// evaluation of the uncommon opcode, but will be almost free to implement.

// _mm_sha256rnds2_epu32 performs two rounds, so this is four.
void round(xint128_t& s0, xint128_t& s1, uint64_t k1, uint64_t k0) NOEXCEPT
{
    // This is actually m + k precomputed for fixed single block padding.
    const auto value = set(k1, k0);

    s1 = _mm_sha256rnds2_epu32(s1, s0, value);
    s0 = _mm_sha256rnds2_epu32(s0, s1, i128::shuffle<0x0e>(value));
}

void round(xint128_t& s0, xint128_t& s1, xint128_t m, uint64_t k1, uint64_t k0) NOEXCEPT
{
    // The sum m + k is computed in the message schedule.
    const auto value = sum(m, set(k1, k0));

    s1 = _mm_sha256rnds2_epu32(s1, s0, value);
    s0 = _mm_sha256rnds2_epu32(s0, s1, i128::shuffle<0x0e>(value));
}

void shift_message(xint128_t& out, xint128_t m) NOEXCEPT
{
    out = _mm_sha256msg1_epu32(out, m);
}

void shift_message(xint128_t m0, xint128_t m1, xint128_t& out) NOEXCEPT
{
    constexpr auto shift = sizeof(uint32_t);
    out = _mm_sha256msg2_epu32(sum(out, align_right<shift>(m1, m0)), m1);
}

void shift_messages(xint128_t& out0, xint128_t m,
    xint128_t& out1) NOEXCEPT
{
    shift_message(out0, m, out1);
    shift_message(out0, m);
}

// endianness
// ----------------------------------------------------------------------------

// Endianness of the buffer/digest should be computed outside of hash function.
// Given the full mutable buffer, can be parallallized and vectorized in place.

void shuffle(xint128_t& s0, xint128_t& s1) NOEXCEPT
{
    const auto t1 = i128::shuffle<0xb1>(s0);
    const auto t2 = i128::shuffle<0x1b>(s1);
    s0 = align_right<8>(t1, t2);
    s1 = blend<15>(t2, t1);
}

void unshuffle(xint128_t& s0, xint128_t& s1) NOEXCEPT
{
    const xint128_t t1 = i128::shuffle<0x1b>(s0);
    const xint128_t t2 = i128::shuffle<0xb1>(s1);
    s0 = blend<15>(t1, t2);
    s1 = align_right<8>(t2, t1);
}

#endif

////void hash_shani(state& state, const blocks& blocks) NOEXCEPT;
void hash_shani(state& state, const block1& blocks) NOEXCEPT
{
    BC_PUSH_WARNING(NO_ARRAY_INDEXING)

    xint128_t m0, m1, m2, m3, so0, so1;

    // From unaligned (public).
    auto s0 = load32x4u(state[0]);
    auto s1 = load32x4u(state[4]);

    // state/SHA is LE, so why bswap?
    // must be treating state as digest.
    shuffle(s0, s1);

    // Each round is four sha rounds.
    // One block in four lanes.
    for (auto& block: blocks)
    {
        // Remember old state.
        so0 = s0;
        so1 = s1;

        // One block loaded 16 bytes (1 uint128) per each of 4 messages.
        // load data and transform.
        m0 = load(block[0]);

        // shift message computes next 4 messages from prevous 4.
        // K: 0xe9b5dba5[3] 0xb5c0fbcfull[2] 0x71374491[1] 0x428a2f98ull[0]
        round(s0, s1, m0, 0xe9b5dba5b5c0fbcfull, 0x71374491428a2f98ull);
        m1 = load(block[16]);
        round(s0, s1, m1, 0xab1c5ed5923f82a4ull, 0x59f111f13956c25bull);
        shift_message(m0, m1); // new m0 from m1
        m2 = load(block[32]);
        round(s0, s1, m2, 0x550c7dc3243185beull, 0x12835b01d807aa98ull);
        shift_message(m1, m2);
        m3 = load(block[48]);

        // shift messages computes next 4 messages from prevous 8.
        round(s0, s1, m3, 0xc19bf1749bdc06a7ull, 0x80deb1fe72be5d74ull);
        shift_messages(m2, m3, m0);
        round(s0, s1, m0, 0x240ca1cc0fc19dc6ull, 0xefbe4786E49b69c1ull);
        shift_messages(m3, m0, m1);
        round(s0, s1, m1, 0x76f988da5cb0a9dcull, 0x4a7484aa2de92c6full);
        shift_messages(m0, m1, m2);
        round(s0, s1, m2, 0xbf597fc7b00327c8ull, 0xa831c66d983e5152ull);
        shift_messages(m1, m2, m3);
        round(s0, s1, m3, 0x1429296706ca6351ull, 0xd5a79147c6e00bf3ull);
        shift_messages(m2, m3, m0);
        round(s0, s1, m0, 0x53380d134d2c6dfcull, 0x2e1b213827b70a85ull);
        shift_messages(m3, m0, m1);
        round(s0, s1, m1, 0x92722c8581c2c92eull, 0x766a0abb650a7354ull);
        shift_messages(m0, m1, m2);
        round(s0, s1, m2, 0xc76c51A3c24b8b70ull, 0xa81a664ba2bfe8a1ull);
        shift_messages(m1, m2, m3);
        round(s0, s1, m3, 0x106aa070f40e3585ull, 0xd6990624d192e819ull);
        shift_messages(m2, m3, m0);
        round(s0, s1, m0, 0x34b0bcb52748774cull, 0x1e376c0819a4c116ull);
        shift_messages(m3, m0, m1);
        round(s0, s1, m1, 0x682e6ff35b9cca4full, 0x4ed8aa4a391c0cb3ull);
        shift_message(m0, m1, m2);
        round(s0, s1, m2, 0x8cc7020884c87814ull, 0x78a5636f748f82eeull);
        shift_message(m1, m2, m3);
        round(s0, s1, m3, 0xc67178f2bef9A3f7ull, 0xa4506ceb90befffaull);

        // Combine with old state.
        s0 = sum(s0, so0);
        s1 = sum(s1, so1);
    }

    // state/SHA is LE, so why bswap?
    // must be treating state as digest.
    unshuffle(s0, s1);

    // To not aligned.
    store32x4u(state[0], s0);
    store32x4u(state[4], s1);

    BC_POP_WARNING()
}

#endif // HAVE_XCPU

#endif // DISABLED

} // namespace sha256
} // namespace system
} // namespace libbitcoin
