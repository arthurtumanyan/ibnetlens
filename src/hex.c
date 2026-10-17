#include "hex.h"

#if defined(__x86_64__) && defined(__GNUC__) && !defined(IBNETLENS_NO_SIMD)
#include <immintrin.h>
#define HAVE_X86_SIMD 1
#endif

bool ibt_hex_simd;

static int nibble(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static size_t hex_scalar(const char *p, uint64_t *out)
{
    uint64_t v = 0;
    size_t n = 0;
    int d;
    while (n < 16 && (d = nibble(p[n])) >= 0) {
        v = (v << 4) | (uint64_t)d;
        n++;
    }
    if (n == 16 && nibble(p[16]) >= 0)
        return 17;
    *out = v;
    return n;
}

#ifdef HAVE_X86_SIMD
/* Shuffle control that right-aligns n digits: 0x80 lanes become zero. */
static const uint8_t align_table[32] __attribute__((aligned(16))) = {
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0,    1,    2,    3,    4,    5,    6,    7,    8,    9,    10,   11,   12,   13,   14,   15,
};

__attribute__((target("ssse3")))
static size_t hex_ssse3(const char *p, uint64_t *out)
{
    __m128i v = _mm_loadu_si128((const __m128i *)p);

    __m128i dig = _mm_sub_epi8(v, _mm_set1_epi8('0'));
    __m128i is_dig = _mm_cmpeq_epi8(_mm_min_epu8(dig, _mm_set1_epi8(9)), dig);

    /* OR 0x20 folds A-F onto a-f; digits already have that bit set */
    __m128i alpha = _mm_sub_epi8(_mm_or_si128(v, _mm_set1_epi8(0x20)), _mm_set1_epi8('a'));
    __m128i is_alpha = _mm_cmpeq_epi8(_mm_min_epu8(alpha, _mm_set1_epi8(5)), alpha);

    unsigned valid = (unsigned)_mm_movemask_epi8(_mm_or_si128(is_dig, is_alpha));
    size_t n = (size_t)__builtin_ctz(~valid | 0x10000u);
    if (n == 0)
        return 0;
    if (n == 16 && nibble(p[16]) >= 0)
        return 17;

    __m128i val = _mm_or_si128(_mm_and_si128(is_dig, dig),
                               _mm_and_si128(is_alpha, _mm_add_epi8(alpha, _mm_set1_epi8(10))));
    val = _mm_shuffle_epi8(val, _mm_loadu_si128((const __m128i *)(align_table + n)));

    __m128i bytes = _mm_maddubs_epi16(val, _mm_set1_epi16(0x0110));
    bytes = _mm_packus_epi16(bytes, bytes);
    *out = __builtin_bswap64((uint64_t)_mm_cvtsi128_si64(bytes));
    return n;
}
#endif

void ibt_hex_init(void)
{
#ifdef HAVE_X86_SIMD
    __builtin_cpu_init();
    ibt_hex_simd = __builtin_cpu_supports("ssse3");
#endif
}

size_t ibt_hex_prefix(const char *p, uint64_t *out)
{
#ifdef HAVE_X86_SIMD
    if (ibt_hex_simd)
        return hex_ssse3(p, out);
#endif
    return hex_scalar(p, out);
}
