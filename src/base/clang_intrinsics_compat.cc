// Temporary build support for the bundled clang-cl toolchain.

#include <emmintrin.h>

#if defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))

#define MOZKEY_CLANG_WEAK __attribute__((weak))

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_loadu_si128(const __m128i* p) {
  __m128i value;
  const auto* source = reinterpret_cast<const unsigned char*>(p);
  auto* destination = reinterpret_cast<unsigned char*>(&value);
  for (int i = 0; i < 16; ++i) destination[i] = source[i];
  return value;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_loadl_epi64(const __m128i* p) {
  __m128i value{};
  const auto* source = reinterpret_cast<const unsigned char*>(p);
  auto* destination = reinterpret_cast<unsigned char*>(&value);
  for (int i = 0; i < 8; ++i) destination[i] = source[i];
  return value;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_set1_epi8(char value) {
  __m128i result;
  for (int i = 0; i < 16; ++i) result.m128i_i8[i] = value;
  return result;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_set1_epi16(short value) {
  __m128i result;
  for (int i = 0; i < 8; ++i) result.m128i_i16[i] = value;
  return result;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_cmpeq_epi8(__m128i lhs, __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 16; ++i) {
    result.m128i_i8[i] = lhs.m128i_i8[i] == rhs.m128i_i8[i] ? -1 : 0;
  }
  return result;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_cmpeq_epi16(__m128i lhs, __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 8; ++i) {
    result.m128i_i16[i] = lhs.m128i_i16[i] == rhs.m128i_i16[i] ? -1 : 0;
  }
  return result;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_cmpgt_epi8(__m128i lhs, __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 16; ++i) {
    result.m128i_i8[i] =
        static_cast<signed char>(lhs.m128i_i8[i]) >
                static_cast<signed char>(rhs.m128i_i8[i])
            ? -1
            : 0;
  }
  return result;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_and_si128(__m128i lhs, __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 16; ++i) {
    result.m128i_u8[i] = lhs.m128i_u8[i] & rhs.m128i_u8[i];
  }
  return result;
}

extern "C" MOZKEY_CLANG_WEAK __m128i _mm_subs_epi8(__m128i lhs, __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 16; ++i) {
    const int difference =
        static_cast<int>(static_cast<signed char>(lhs.m128i_i8[i])) -
        static_cast<int>(static_cast<signed char>(rhs.m128i_i8[i]));
    result.m128i_i8[i] = static_cast<signed char>(
        difference < -128 ? -128 : difference > 127 ? 127 : difference);
  }
  return result;
}

extern "C" MOZKEY_CLANG_WEAK void _mm_storeu_si128(__m128i* p, __m128i value) {
  const auto* source = reinterpret_cast<const unsigned char*>(&value);
  auto* destination = reinterpret_cast<unsigned char*>(p);
  for (int i = 0; i < 16; ++i) destination[i] = source[i];
}

extern "C" MOZKEY_CLANG_WEAK int _mm_movemask_epi8(__m128i value) {
  int mask = 0;
  for (int i = 0; i < 16; ++i) {
    mask |= ((static_cast<unsigned char>(value.m128i_i8[i]) >> 7) << i);
  }
  return mask;
}

#endif
