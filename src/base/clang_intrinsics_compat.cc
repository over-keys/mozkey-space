// Fallback implementations for the bundled clang-cl toolchain.
//
// Do not define functions named _mm_* here.  Clang's intrinsic headers already
// declare those names, and redeclaring them breaks with newer Clang versions.
// The linker alternates below are used only when clang emits an unresolved
// external reference instead of an inline intrinsic.

#include <emmintrin.h>

#if defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))

#define MOZKEY_CLANG_COMPAT __declspec(noinline) __attribute__((weak))

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_loadu_si128(
    const __m128i* p) {
  __m128i value;
  const auto* source = reinterpret_cast<const unsigned char*>(p);
  auto* destination = reinterpret_cast<unsigned char*>(&value);
  for (int i = 0; i < 16; ++i) destination[i] = source[i];
  return value;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_loadl_epi64(
    const __m128i* p) {
  __m128i value{};
  const auto* source = reinterpret_cast<const unsigned char*>(p);
  auto* destination = reinterpret_cast<unsigned char*>(&value);
  for (int i = 0; i < 8; ++i) destination[i] = source[i];
  return value;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_set1_epi8(char value) {
  __m128i result;
  for (int i = 0; i < 16; ++i) result.m128i_i8[i] = value;
  return result;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_set1_epi16(short value) {
  __m128i result;
  for (int i = 0; i < 8; ++i) result.m128i_i16[i] = value;
  return result;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_cmpeq_epi8(__m128i lhs,
                                                               __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 16; ++i) {
    result.m128i_i8[i] = lhs.m128i_i8[i] == rhs.m128i_i8[i] ? -1 : 0;
  }
  return result;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_cmpeq_epi16(
    __m128i lhs, __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 8; ++i) {
    result.m128i_i16[i] = lhs.m128i_i16[i] == rhs.m128i_i16[i] ? -1 : 0;
  }
  return result;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_cmpgt_epi8(__m128i lhs,
                                                               __m128i rhs) {
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

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_and_si128(__m128i lhs,
                                                             __m128i rhs) {
  __m128i result;
  for (int i = 0; i < 16; ++i) {
    result.m128i_u8[i] = lhs.m128i_u8[i] & rhs.m128i_u8[i];
  }
  return result;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_subs_epi8(__m128i lhs,
                                                              __m128i rhs) {
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

extern "C" MOZKEY_CLANG_COMPAT void mozkey_mm_storeu_si128(__m128i* p,
                                                              __m128i value) {
  const auto* source = reinterpret_cast<const unsigned char*>(&value);
  auto* destination = reinterpret_cast<unsigned char*>(p);
  for (int i = 0; i < 16; ++i) destination[i] = source[i];
}

extern "C" MOZKEY_CLANG_COMPAT int mozkey_mm_movemask_epi8(__m128i value) {
  int mask = 0;
  for (int i = 0; i < 16; ++i) {
    mask |= ((static_cast<unsigned char>(value.m128i_i8[i]) >> 7) << i);
  }
  return mask;
}

#pragma comment(linker, "/alternatename:_mm_loadu_si128=mozkey_mm_loadu_si128")
#pragma comment(linker, "/alternatename:_mm_loadl_epi64=mozkey_mm_loadl_epi64")
#pragma comment(linker, "/alternatename:_mm_set1_epi8=mozkey_mm_set1_epi8")
#pragma comment(linker, "/alternatename:_mm_set1_epi16=mozkey_mm_set1_epi16")
#pragma comment(linker, "/alternatename:_mm_cmpeq_epi8=mozkey_mm_cmpeq_epi8")
#pragma comment(linker, "/alternatename:_mm_cmpeq_epi16=mozkey_mm_cmpeq_epi16")
#pragma comment(linker, "/alternatename:_mm_cmpgt_epi8=mozkey_mm_cmpgt_epi8")
#pragma comment(linker, "/alternatename:_mm_and_si128=mozkey_mm_and_si128")
#pragma comment(linker, "/alternatename:_mm_subs_epi8=mozkey_mm_subs_epi8")
#pragma comment(linker, "/alternatename:_mm_storeu_si128=mozkey_mm_storeu_si128")
#pragma comment(linker, "/alternatename:_mm_movemask_epi8=mozkey_mm_movemask_epi8")

#endif  // defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
