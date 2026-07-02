/*
 * Copyright (c) 2024 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */
#include "common/checksum_impl.h"

#include "vesal/log_setting.h"

#ifdef VESAL_HAVE_PCLMULQDQ
#ifdef VESAL_HAVE_SSE4_2
#include <immintrin.h>
#endif
#endif

namespace vesal {
// The following crc32 functions are from bytelib, to verify the QAT-based CRC32 results.
namespace {
typedef uint32_t (*ComputeCRC32_Impl)(uint32_t, const char*, size_t);

uint32_t ComputeCRC32_Accelerate(uint32_t init_crc, const char* buf, size_t size) {
    uint32_t crc = init_crc;
    const uint64_t* p = (const uint64_t*)buf;
    const uint64_t* end = p + (size >> 3);
    uint64_t crc64 = crc;
    while (p < end) {
#if defined(__x86_64__)
        asm volatile("crc32q %[buf], %[crc]\n\t" : [crc] "=r"(crc64) : "0"(crc64), [buf] "r"(*p));
#elif __aarch64__
        asm volatile("crc32cx %w[c], %w[c], %x[v]" : [c] "=r"(crc64) : "0"(crc64), [v] "r"(*p));
#else
#error "Unknown Architecture"
#endif
        ++p;
    }
    crc = (uint32_t)crc64;
    const char* buf_end = buf + size;
    buf = (const char*)p;
    while (buf < buf_end) {
#if defined(__x86_64__)
        asm volatile("crc32b %[buf], %[crc]\n\t" : [crc] "=r"(crc) : "0"(crc), [buf] "r"(*buf));
#elif __aarch64__
        asm volatile("crc32cb %w[c], %w[c], %w[v]" : [c] "=r"(crc) : "0"(crc), [v] "r"(*buf));
#else
#error "Unknown Architecture"
#endif
        ++buf;
    }
    return crc;
}

ComputeCRC32_Impl Get_ComputeCRC32_Impl() {
#if defined(__x86_64__)
    static const uint32_t k_SSE4_2 = (1 << 20);
    int level = 1;
    uint64_t a, b, c, d;
    asm("xchg %%rbx, %1\n\t"
        "cpuid\n\t"
        "xchg %%rbx, %1\n\t"
        : "=a"(a), "=r"(b), "=c"(c), "=d"(d)
        : "0"(level));
    return (c & k_SSE4_2) ? &ComputeCRC32_Accelerate : nullptr;
#elif __aarch64__
    // TODO(sjj): check if the processor supports crc32 instruction.
    return &ComputeCRC32_Accelerate;
#else
#error "Unknown Architecture"
#endif
}

static const uint32_t kCRC32X64NTable[64 * 6] = {
    0x80000000, 0x00800000, 0x00008000, 0x00000080, 0x82f63b78, 0xfbc3faf9, 0x8b277743, 0x52a0c93f,
    0x6ea2d55c, 0x1c08b7d6, 0xf56e0ef4, 0x34019664, 0xa66805eb, 0x7a1f6b24, 0xe75d06aa, 0xc94ec098,
    0x18b8ea18, 0x9a9f274a, 0x2a03aaa3, 0xb13145f8, 0x790606ff, 0xad045557, 0x8542ba6d, 0xde6b9d0b,
    0x9957c0a6, 0x8473058e, 0x2e0af75a, 0x3ae9f81c, 0x5d27e147, 0x95ec5eb6, 0x9421797f, 0x2f1f4950,
    0x510ac59a, 0xf91bdeea, 0x882b9bfc, 0xbea58b3e, 0x9c25531d, 0xafeaaeef, 0xbd8c7e90, 0x92157069,
    0x19e65dde, 0x7fb2b8d1, 0x21c7d010, 0x107f00bf, 0xec1631ed, 0x5cf4f2f8, 0x79ebc348, 0xcbdbaeb0,
    0xb2dea967, 0xb5be2920, 0x200830f7, 0x278403ae, 0x0e148e82, 0x63c35f01, 0xf208405c, 0x1c941d43,
    0x52377a55, 0x6486f9b5, 0x8780e02c, 0x6d79c1ee, 0x4f256efc, 0xbe6285cb, 0x5abaef7a, 0x1a20c6da,
    0x80000000, 0xb82be955, 0xb8fdb1e7, 0x18e4a304, 0x88e56f72, 0x9030a49c, 0x67db2c4a, 0xe744dcb2,
    0x74c360a4, 0x8e9906a7, 0xb469724f, 0x24d84074, 0x631bb273, 0xee2a2d56, 0x654f84e7, 0x629069c6,
    0xe4172b16, 0xd3cb2137, 0x4f47bf52, 0x35202632, 0x71892b1b, 0xd27285db, 0x69c2af09, 0xd271158e,
    0x835305c9, 0xeaa500ac, 0x0e36a726, 0x46951631, 0x196b1eae, 0x584ea70d, 0x15e8653c, 0xb5a65826,
    0x0d65762a, 0xebfcce28, 0xe6aef08f, 0x013ee85b, 0xafc81338, 0x61c30b85, 0xe31e7f5b, 0x5e3750cd,
    0xb5a50ab7, 0x1977c75d, 0x46bf959e, 0xe78cd046, 0xf373c3ac, 0x950979dd, 0xe31b4008, 0xc726c863,
    0x5f60970f, 0xf977f383, 0x9e7b1c10, 0xea2110f8, 0xd46d3063, 0xe2220529, 0xebd59d26, 0xedbe69a1,
    0x3a5275ea, 0x1a2bbe21, 0xd597ad7e, 0xcb8c45a6, 0x02331c01, 0xe8639712, 0xa957c550, 0xbcfda51c,
    0x80000000, 0x35d73a62, 0x28461564, 0x43eefc9f, 0xbf455269, 0x5edcdeb9, 0x65059450, 0x4e6f41a4,
    0xe2ea32dc, 0xf5b95b32, 0x695a4c87, 0xf204502c, 0x9a4f01b6, 0x941a0916, 0x4303cb97, 0x4c3d3d65,
    0xfe7740e6, 0xf0cb95d2, 0xab4c722a, 0xba35e2e1, 0x99569602, 0x294060e6, 0xd65a1d78, 0x18103ff9,
    0x78b57ca2, 0x97b3a098, 0x30a8edff, 0x3fb4e611, 0x999dcda1, 0xd9dd2e27, 0x5918f954, 0x8ee73513,
    0xf946610b, 0xa5f4742d, 0x424b8555, 0xd017c143, 0xc274d1e2, 0x0280b04e, 0xd25bb5e0, 0x552a0918,
    0xf5942afb, 0x55d7a3f8, 0xd1b533ee, 0x9fa8def5, 0x24c42589, 0x5988dbb3, 0xa7e1cab4, 0xebaee1b4,
    0xa0a51f5f, 0xbd1a9096, 0x1ced05cc, 0x6748e206, 0x506431b0, 0x6c399e24, 0x30af367f, 0x129ee2f5,
    0x8fd57416, 0x85ee5ed0, 0x97504a07, 0x52e710c7, 0x4ea3e89b, 0xde8567b5, 0xb5c84194, 0x80526793,
    0x80000000, 0x3c204f8f, 0x538586e3, 0x683988b2, 0x59726915, 0x30c6966d, 0x5be8b36e, 0x42c5623c,
    0x734d5309, 0x1084fed0, 0x68a5f0f9, 0x1573fe60, 0xdb1b3315, 0xe0a82539, 0xe5c304f7, 0xce8068d1,
    0xbc1ac763, 0x47514f16, 0xf7f919a7, 0x5005a217, 0x11b31f9d, 0x496f21b8, 0x0bc2a806, 0xbba39916,
    0x5414bce3, 0x401f0313, 0xe0ee85d5, 0x4fb6f7ac, 0x54f2f77b, 0x738279cb, 0x6b645133, 0x05a44de1,
    0x7d0722cc, 0x8ca9c7c7, 0x50806311, 0x58ba16a0, 0xbae70d08, 0xde3a8229, 0x6652a6cc, 0x36b6490b,
    0x238acb2b, 0xe3f9a08b, 0x23a7923f, 0xb0609b9b, 0xd2074e2b, 0x342f6e39, 0x5bcf2cf2, 0x8096907d,
    0x546dfa6e, 0x75337ed4, 0x9c1de396, 0x91e7afe7, 0x3f76a96c, 0x00032b5e, 0x1a2a93d0, 0x3b0c8728,
    0xa3aa3946, 0x9b170192, 0xcc33e9f6, 0x4152bdb8, 0xf054ca48, 0x0369fe6a, 0x6ec7bef8, 0x3fe8b9ff,
    0x80000000, 0xd289cabe, 0xe94ca9bc, 0x6ac5538f, 0x05b74f3f, 0x9e0b2298, 0x14de47cd, 0x3a9fb07d,
    0xa51e1f42, 0xfe578179, 0x038d778c, 0x5fe3cf44, 0xb7c64e40, 0x20cbb8e3, 0x4f04f803, 0x56745358,
    0x40000000, 0x6944e55f, 0x74a654de, 0xb79492bf, 0x802d9ce7, 0x4f05914c, 0x8899189e, 0x9fb9e346,
    0x528f0fa1, 0xfdddfbc4, 0x01c6bbc6, 0x2ff1e7a2, 0x5be32720, 0x9293e709, 0xa5744779, 0x2b3a29ac,
    0x20000000, 0xb65449d7, 0x3a532a6f, 0xd93c7227, 0xc2e0f50b, 0x2782c8a6, 0x444c8c4f, 0x4fdcf1a3,
    0xabb1bca8, 0x7eeefde2, 0x00e35de3, 0x17f8f3d1, 0x2df19390, 0xcbbfc8fc, 0xd04c18c4, 0x159d14d6,
    0x10000000, 0xd9dc1f93, 0x9fdfae4f, 0xee68026b, 0xe38641fd, 0x13c16453, 0xa0d07d5f, 0xa51843a9,
    0x55d8de54, 0x3f777ef1, 0x82879589, 0x890a4290, 0x16f8c9c8, 0x65dfe47e, 0x68260c62, 0x0ace8a6b,
    0x80000000, 0x08000000, 0x00800000, 0x00080000};

uint32_t crc32Mult(uint32_t a, uint32_t b);

uint32_t CRC32CombineGen(size_t len) {
    const uint32_t m = 1 << 31;
    uint32_t p = 1 << 31;
    int i = 0;
    if (m <= len) {
        len = (len & (m - 1)) + (len >> 31);
        if (m <= len) {
            len = (len & (m - 1)) + (len >> 31);
        }
    }
    if (len == 0) {
        return p;
    }
    for (; !(len & 63); len >>= 6, i++) {
    }
    p = kCRC32X64NTable[i * 64 + (len & 63)];
    for (len >>= 6, i++; len; len >>= 6, i++) {
        if (len & 63) {
            p = crc32Mult(kCRC32X64NTable[i * 64 + (len & 63)], p);
        }
    }
    return p;
}

uint32_t CRC32CombineOp(uint32_t crc1, uint32_t crc2, uint32_t op) {
    return crc32Mult(op, crc1) ^ crc2;
}

#ifdef VESAL_HAVE_PCLMULQDQ
#ifdef VESAL_HAVE_SSE4_2
#ifdef __clang__
#pragma clang attribute push(__attribute__((target("pclmul, sse4.2"))), \
                             apply_to = function)  // NOLINT
#else
#pragma GCC push_options
#pragma GCC target("pclmul", "sse4.2")
#endif
uint32_t crc32mul_pclmul(uint32_t a, uint32_t b) {
    __m128i va = _mm_set_epi32(0, 0, 0, a);
    __m128i vb = _mm_set_epi32(0, 0, 0, b);
    __m128i v = _mm_clmulepi64_si128(va, vb, 0);
    uint64_t m = _mm_extract_epi64(v, 0);
    uint32_t lo = m >> 31;
    uint32_t hi = (m & ((1U << 31) - 1)) << 1;
    return _mm_crc32_u32(hi, 0) ^ lo;
}
uint32_t crc32ff_pclmul(uint32_t crc, size_t len) {
    const uint32_t m = 1 << 31;
    if (m <= len) {
        len = (len & (m - 1)) + (len >> 31);
        if (m <= len) {
            len = (len & (m - 1)) + (len >> 31);
        }
    }
    if (len == 0) {
        return crc;
    }
    __m128i v = _mm_set_epi32(0, 0, 0, crc);
    __m128i k;
    __m128i p;
    int i = 0;
    int d = 0;
    for (; len; len >>= 6, i++) {
        if (len & 63) {
            int64_t c0 = kCRC32X64NTable[i * 64 + (len & 63)];
            int64_t c1 = 0;
            if (d < 2) {
                k = _mm_set_epi64x(0, c0 << 1);
                v = _mm_clmulepi64_si128(v, k, 0);
                d++;
                continue;
            }
            c1 = _mm_crc32_u64(c0, 0);
            k = _mm_set_epi64x(c1 << 1, c0 << 1);
            v = _mm_slli_si128(v, 4);
            p = _mm_clmulepi64_si128(v, k, 0x10);  // u0 * c * x65
            v = _mm_clmulepi64_si128(v, k, 0x01);  // u1 * c * x1
            v = _mm_xor_si128(v, p);
        }
    }
    // reduce to 32-bit
    if (d < 2) {
        uint64_t u0 = _mm_extract_epi64(v, 0);
        uint32_t lo = u0 >> 32;
        uint32_t hi = u0 & ~(uint32_t)0;
        return _mm_crc32_u32(hi, 0) ^ lo;
    } else {
        uint64_t u0 = _mm_extract_epi64(v, 0);
        uint64_t u1 = _mm_extract_epi64(v, 1);
        return _mm_crc32_u64(0, u0) ^ u1;
    }
}
#ifdef __clang__
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif
#endif
#endif

uint32_t crc32Mult(uint32_t a, uint32_t b) {
#ifdef VESAL_HAVE_PCLMULQDQ
#ifdef VESAL_HAVE_SSE4_2
    if (__builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse4.2")) {
        return crc32mul_pclmul(a, b);
    }
#endif
#endif
    uint32_t m = 1 << 31;
    uint32_t p = 0;
    for (;;) {
        if (a & m) {
            p ^= b;
            if ((a & (m - 1)) == 0)
                break;
        }
        m >>= 1;
        b = b & 1 ? (b >> 1) ^ kCrc32cPolyReflect : b >> 1;
    }
    return p;
}

}  // namespace

// used for lz4 header xxhash checksum
// copied from bytelib
uint32_t XxHash(const char* ptr, size_t size) {
    // Primes
    static const uint32_t k1 = 2654435761U;
    static const uint32_t k2 = 2246822519U;
    static const uint32_t k3 = 3266489917U;
    static const uint32_t k4 = 668265263U;
    static const uint32_t k5 = 374761393U;

    const uint32_t* p = reinterpret_cast<const uint32_t*>(ptr);
    const uint32_t* end = reinterpret_cast<const uint32_t*>(ptr + size);
    uint32_t h = k5;

#define XX_ROT32(x, r) (((x) << (r)) | ((x) >> (32 - (r))))
    if (size >= 16) {
        const uint32_t* limit = end - 4;
        uint32_t v1 = k1 + k2;
        uint32_t v2 = k2;
        uint32_t v3 = 0;
        uint32_t v4 = 0 - k1;
        do {
            v1 += *p++ * k2, v1 = XX_ROT32(v1, 13), v1 *= k1;
            v2 += *p++ * k2, v2 = XX_ROT32(v2, 13), v2 *= k1;
            v3 += *p++ * k2, v3 = XX_ROT32(v3, 13), v3 *= k1;
            v4 += *p++ * k2, v4 = XX_ROT32(v4, 13), v4 *= k1;
        } while (p <= limit);
        h = XX_ROT32(v1, 1) + XX_ROT32(v2, 7) + XX_ROT32(v3, 12) + XX_ROT32(v4, 18);
    }

    h += size;
    while (p <= end - 1) {
        h += *p++ * k3;
        h = XX_ROT32(h, 17) * k4;
    }

    const char* q = reinterpret_cast<const char*>(p);
    const char* e = reinterpret_cast<const char*>(end);
    while (q < e) {
        h += *q++ * k5;
        h = XX_ROT32(h, 11) * k1;
    }

    h ^= h >> 15;
    h *= k2;
    h ^= h >> 13;
    h *= k3;
    h ^= h >> 16;
#undef XX_ROT32
    return h;
}

uint32_t ComputeCRC32(uint32_t init_crc, const char* buf, size_t size) {
    static ComputeCRC32_Impl compute_crc32_impl = Get_ComputeCRC32_Impl();
    VESAL_CHECK(compute_crc32_impl);
    return compute_crc32_impl(init_crc, buf, size);
}

uint32_t CRC32Combine(uint32_t crc1, uint32_t crc2, size_t size2) {
#ifdef VESAL_HAVE_PCLMULQDQ
#ifdef VESAL_HAVE_SSE4_2
    if (__builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse4.2")) {
        return crc32ff_pclmul(crc1, size2) ^ crc2;
    }
#endif
#endif
    return CRC32CombineOp(crc1, crc2, CRC32CombineGen(size2));
}

}  // namespace vesal