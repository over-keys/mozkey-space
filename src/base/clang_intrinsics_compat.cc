// Fallback implementations for the bundled clang-cl toolchain.
//
// Do not define functions named _mm_* here.  Clang's intrinsic headers already
// declare those names, and redeclaring them breaks with newer Clang versions.
// The linker alternates below are used only when clang emits an unresolved
// external reference instead of an inline intrinsic.

#include <emmintrin.h>

#include <array>
#include <cstdint>
#include <cstring>

#if defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))

// This translation unit is linked once through the public compatibility
// library.  Keep the fallback definitions strong: clang's weak COFF symbols
// are emitted as weak externals, which can leave the x86 linker unable to
// resolve an /alternatename target.
#define MOZKEY_CLANG_COMPAT __declspec(noinline)

static_assert(sizeof(__m128i) == 16);

template <typename Lane, std::size_t Count>
std::array<Lane, Count> ToLanes(__m128i value) {
  static_assert(sizeof(std::array<Lane, Count>) == sizeof(__m128i));
  std::array<Lane, Count> lanes;
  std::memcpy(lanes.data(), &value, sizeof(value));
  return lanes;
}

template <typename Lane, std::size_t Count>
__m128i FromLanes(const std::array<Lane, Count>& lanes) {
  static_assert(sizeof(std::array<Lane, Count>) == sizeof(__m128i));
  __m128i value{};
  std::memcpy(&value, lanes.data(), sizeof(value));
  return value;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_loadu_si128(
    const __m128i* p) {
  __m128i value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_loadl_epi64(
    const __m128i* p) {
  __m128i value{};
  std::memcpy(&value, p, sizeof(std::int64_t));
  return value;
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_set1_epi8(char value) {
  std::array<std::int8_t, 16> lanes;
  lanes.fill(static_cast<std::int8_t>(value));
  return FromLanes(lanes);
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_set1_epi16(short value) {
  std::array<std::int16_t, 8> lanes;
  lanes.fill(static_cast<std::int16_t>(value));
  return FromLanes(lanes);
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_cmpeq_epi8(__m128i lhs,
                                                               __m128i rhs) {
  const auto lhs_lanes = ToLanes<std::int8_t, 16>(lhs);
  const auto rhs_lanes = ToLanes<std::int8_t, 16>(rhs);
  std::array<std::int8_t, 16> result;
  for (int i = 0; i < 16; ++i) {
    result[i] = lhs_lanes[i] == rhs_lanes[i] ? -1 : 0;
  }
  return FromLanes(result);
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_cmpeq_epi16(
    __m128i lhs, __m128i rhs) {
  const auto lhs_lanes = ToLanes<std::int16_t, 8>(lhs);
  const auto rhs_lanes = ToLanes<std::int16_t, 8>(rhs);
  std::array<std::int16_t, 8> result;
  for (int i = 0; i < 8; ++i) {
    result[i] = lhs_lanes[i] == rhs_lanes[i] ? -1 : 0;
  }
  return FromLanes(result);
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_cmpgt_epi8(__m128i lhs,
                                                               __m128i rhs) {
  const auto lhs_lanes = ToLanes<std::int8_t, 16>(lhs);
  const auto rhs_lanes = ToLanes<std::int8_t, 16>(rhs);
  std::array<std::int8_t, 16> result;
  for (int i = 0; i < 16; ++i) {
    result[i] = lhs_lanes[i] > rhs_lanes[i] ? -1 : 0;
  }
  return FromLanes(result);
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_and_si128(__m128i lhs,
                                                             __m128i rhs) {
  const auto lhs_lanes = ToLanes<std::uint8_t, 16>(lhs);
  const auto rhs_lanes = ToLanes<std::uint8_t, 16>(rhs);
  std::array<std::uint8_t, 16> result;
  for (int i = 0; i < 16; ++i) {
    result[i] = lhs_lanes[i] & rhs_lanes[i];
  }
  return FromLanes(result);
}

extern "C" MOZKEY_CLANG_COMPAT __m128i mozkey_mm_subs_epi8(__m128i lhs,
                                                              __m128i rhs) {
  const auto lhs_lanes = ToLanes<std::int8_t, 16>(lhs);
  const auto rhs_lanes = ToLanes<std::int8_t, 16>(rhs);
  std::array<std::int8_t, 16> result;
  for (int i = 0; i < 16; ++i) {
    const int difference = static_cast<int>(lhs_lanes[i]) -
                           static_cast<int>(rhs_lanes[i]);
    result[i] = static_cast<std::int8_t>(
        difference < -128 ? -128 : difference > 127 ? 127 : difference);
  }
  return FromLanes(result);
}

extern "C" MOZKEY_CLANG_COMPAT void mozkey_mm_storeu_si128(__m128i* p,
                                                              __m128i value) {
  const auto* source = reinterpret_cast<const unsigned char*>(&value);
  auto* destination = reinterpret_cast<unsigned char*>(p);
  for (int i = 0; i < 16; ++i) destination[i] = source[i];
}

extern "C" MOZKEY_CLANG_COMPAT int mozkey_mm_movemask_epi8(__m128i value) {
  const auto lanes = ToLanes<std::int8_t, 16>(value);
  int mask = 0;
  for (int i = 0; i < 16; ++i) {
    mask |= ((static_cast<std::uint8_t>(lanes[i]) >> 7) << i);
  }
  return mask;
}

// C symbols in 32-bit COFF have a leading underscore.  The unresolved
// intrinsic names therefore have two underscores (_mm_* + decoration), while
// the fallback symbols have one.  x64 does not add that extra decoration.
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:__mm_loadu_si128=_mozkey_mm_loadu_si128")
#pragma comment(linker, "/alternatename:__mm_loadl_epi64=_mozkey_mm_loadl_epi64")
#pragma comment(linker, "/alternatename:__mm_set1_epi8=_mozkey_mm_set1_epi8")
#pragma comment(linker, "/alternatename:__mm_set1_epi16=_mozkey_mm_set1_epi16")
#pragma comment(linker, "/alternatename:__mm_cmpeq_epi8=_mozkey_mm_cmpeq_epi8")
#pragma comment(linker, "/alternatename:__mm_cmpeq_epi16=_mozkey_mm_cmpeq_epi16")
#pragma comment(linker, "/alternatename:__mm_cmpgt_epi8=_mozkey_mm_cmpgt_epi8")
#pragma comment(linker, "/alternatename:__mm_and_si128=_mozkey_mm_and_si128")
#pragma comment(linker, "/alternatename:__mm_subs_epi8=_mozkey_mm_subs_epi8")
#pragma comment(linker, "/alternatename:__mm_storeu_si128=_mozkey_mm_storeu_si128")
#pragma comment(linker, "/alternatename:__mm_movemask_epi8=_mozkey_mm_movemask_epi8")
#else
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
#endif

#endif  // defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
