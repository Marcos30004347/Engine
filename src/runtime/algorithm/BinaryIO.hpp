#pragma once
#include <cstdint>
#include <cstdio>
#include <string.h>

namespace virtualgeometry
{

inline bool isLittleEndian()
{
  uint16_t v = 1;
  return *reinterpret_cast<uint8_t *>(&v) == 1;
}

template <typename T> inline T byteswap(T v);

template <> inline uint16_t byteswap(uint16_t v)
{
  return (v >> 8) | (v << 8);
}

template <> inline uint32_t byteswap(uint32_t v)
{
  return (v >> 24) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) | (v << 24);
}

template <> inline uint64_t byteswap(uint64_t v)
{
  return (uint64_t(byteswap(uint32_t(v))) << 32) | byteswap(uint32_t(v >> 32));
}

inline void write_u32(FILE *f, uint32_t v)
{
  if (!isLittleEndian())
    v = byteswap(v);
  fwrite(&v, sizeof(v), 1, f);
}

inline void write_u64(FILE *f, uint64_t v)
{
  if (!isLittleEndian())
    v = byteswap(v);
  fwrite(&v, sizeof(v), 1, f);
}

inline void write_f32(FILE *f, float v)
{
  static_assert(sizeof(float) == 4, "IEEE float required");
  uint32_t u;
  memcpy(&u, &v, 4);
  write_u32(f, u);
}

inline uint32_t read_u32(FILE *f)
{
  uint32_t v;
  fread(&v, sizeof(v), 1, f);
  return isLittleEndian() ? v : byteswap(v);
}

inline uint64_t read_u64(FILE *f)
{
  uint64_t v;
  fread(&v, sizeof(v), 1, f);
  return isLittleEndian() ? v : byteswap(v);
}

inline float read_f32(FILE *f)
{
  uint32_t u = read_u32(f);
  float v;
  memcpy(&v, &u, 4);
  return v;
}

} // namespace virtualgeometry
