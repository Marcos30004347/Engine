#pragma once

#include <type_traits>

namespace lib
{
template <typename T> inline unsigned countrZero(T x)
{
  static_assert(std::is_unsigned<T>::value, "countr_zero requires unsigned type");

  if (x == 0)
    return std::numeric_limits<T>::digits;

#if defined(__clang__) || defined(__GNUC__)
  if (sizeof(T) <= sizeof(unsigned int))
    return __builtin_ctz(static_cast<unsigned int>(x));
  else if (sizeof(T) <= sizeof(unsigned long))
    return __builtin_ctzl(static_cast<unsigned long>(x));
  else
    return __builtin_ctzll(static_cast<unsigned long long>(x));

#elif defined(_MSC_VER)
  unsigned long index;

  if (sizeof(T) <= sizeof(unsigned long))
  {
    _BitScanForward(&index, static_cast<unsigned long>(x));
    return static_cast<unsigned>(index);
  }
  else
  {
#if defined(_M_X64) || defined(_M_ARM64)
    _BitScanForward64(&index, static_cast<unsigned long long>(x));
    return static_cast<unsigned>(index);
#else
    // 32-bit MSVC fallback
    unsigned long low = static_cast<unsigned long>(x);
    if (low != 0)
    {
      _BitScanForward(&index, low);
      return static_cast<unsigned>(index);
    }
    _BitScanForward(&index, static_cast<unsigned long>(x >> 32));
    return static_cast<unsigned>(index + 32);
#endif
  }
#else
  // Portable fallback (De Bruijn)
  static const int table[32] = {0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8, 31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9};

  return table[((x & -x) * 0x077CB531u) >> 27];
#endif
}

} // namespace lib