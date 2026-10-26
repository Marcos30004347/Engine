#include "algorithm/BinaryIO.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

using namespace virtualgeometry;

bool test_endianness_detection()
{
  std::cout << "Testing endianness detection... ";

  bool is_little = isLittleEndian();

  uint16_t test_val = 0x1234;
  uint8_t *bytes = reinterpret_cast<uint8_t *>(&test_val);

  bool detected_little = (bytes[0] == 0x34);

  if (is_little != detected_little)
  {
    std::cerr << "FAILED (detection mismatch)\n";
    return false;
  }

  std::cout << "PASSED (system is " << (is_little ? "little" : "big") << "-endian)\n";
  return true;
}

bool test_byteswap_u16()
{
  std::cout << "Testing uint16_t byteswap... ";

  uint16_t val = 0x1234;
  uint16_t swapped = byteswap(val);
  uint16_t expected = 0x3412;

  if (swapped != expected)
  {
    std::cerr << "FAILED (0x" << std::hex << swapped << " != 0x" << expected << ")\n";
    return false;
  }

  if (byteswap(swapped) != val)
  {
    std::cerr << "FAILED (double swap)\n";
    return false;
  }

  std::cout << "PASSED\n";
  return true;
}

bool test_byteswap_u32()
{
  std::cout << "Testing uint32_t byteswap... ";

  uint32_t val = 0x12345678;
  uint32_t swapped = byteswap(val);
  uint32_t expected = 0x78563412;

  if (swapped != expected)
  {
    std::cerr << "FAILED (0x" << std::hex << swapped << " != 0x" << expected << ")\n";
    return false;
  }

  if (byteswap(swapped) != val)
  {
    std::cerr << "FAILED (double swap)\n";
    return false;
  }

  std::cout << "PASSED\n";
  return true;
}

bool test_byteswap_u64()
{
  std::cout << "Testing uint64_t byteswap... ";

  uint64_t val = 0x123456789ABCDEF0ULL;
  uint64_t swapped = byteswap(val);
  uint64_t expected = 0xF0DEBC9A78563412ULL;

  if (swapped != expected)
  {
    std::cerr << "FAILED\n";
    return false;
  }

  if (byteswap(swapped) != val)
  {
    std::cerr << "FAILED (double swap)\n";
    return false;
  }

  std::cout << "PASSED\n";
  return true;
}

bool test_file_io_u32()
{
  std::cout << "Testing uint32_t file I/O... ";

  const char *filename = "test_u32.bin";

  // Write values
  FILE *f = fopen(filename, "wb");
  if (!f)
  {
    std::cerr << "FAILED (open for write)\n";
    return false;
  }

  uint32_t test_values[] = {0, 1, 0x12345678, 0xFFFFFFFF, 0x80000000};

  for (uint32_t val : test_values)
  {
    write_u32(f, val);
  }

  fclose(f);

  f = fopen(filename, "rb");
  if (!f)
  {
    std::cerr << "FAILED (open for read)\n";
    return false;
  }

  for (uint32_t expected : test_values)
  {
    uint32_t val = read_u32(f);
    if (val != expected)
    {
      std::cerr << "FAILED (0x" << std::hex << val << " != 0x" << expected << ")\n";
      fclose(f);
      std::remove(filename);
      return false;
    }
  }

  fclose(f);
  std::remove(filename);

  std::cout << "PASSED\n";
  return true;
}

bool test_file_io_u64()
{
  std::cout << "Testing uint64_t file I/O... ";

  const char *filename = "test_u64.bin";

  FILE *f = fopen(filename, "wb");
  if (!f)
  {
    std::cerr << "FAILED (open for write)\n";
    return false;
  }

  uint64_t test_values[] = {0, 1, 0x123456789ABCDEF0ULL, 0xFFFFFFFFFFFFFFFFULL, 0x8000000000000000ULL};

  for (uint64_t val : test_values)
  {
    write_u64(f, val);
  }

  fclose(f);

  f = fopen(filename, "rb");
  if (!f)
  {
    std::cerr << "FAILED (open for read)\n";
    return false;
  }

  for (uint64_t expected : test_values)
  {
    uint64_t val = read_u64(f);
    if (val != expected)
    {
      std::cerr << "FAILED\n";
      fclose(f);
      std::remove(filename);
      return false;
    }
  }

  fclose(f);
  std::remove(filename);

  std::cout << "PASSED\n";
  return true;
}

bool test_file_io_f32()
{
  std::cout << "Testing float file I/O... ";

  const char *filename = "test_f32.bin";

  FILE *f = fopen(filename, "wb");
  if (!f)
  {
    std::cerr << "FAILED (open for write)\n";
    return false;
  }

  float test_values[] = {0.0f, 1.0f, -1.0f, 3.14159265f, 1e10f, 1e-10f, -123.456f};

  for (float val : test_values)
  {
    write_f32(f, val);
  }

  fclose(f);

  f = fopen(filename, "rb");
  if (!f)
  {
    std::cerr << "FAILED (open for read)\n";
    return false;
  }

  for (float expected : test_values)
  {
    float val = read_f32(f);

    // For floating point, we need to handle representation exactly
    uint32_t val_bits, expected_bits;
    memcpy(&val_bits, &val, sizeof(float));
    memcpy(&expected_bits, &expected, sizeof(float));

    if (val_bits != expected_bits)
    {
      std::cerr << "FAILED (" << val << " != " << expected << ")\n";
      fclose(f);
      std::remove(filename);
      return false;
    }
  }

  fclose(f);
  std::remove(filename);

  std::cout << "PASSED\n";
  return true;
}

bool test_mixed_file_io()
{
  std::cout << "Testing mixed type file I/O... ";

  const char *filename = "test_mixed.bin";

  // Write mixed types
  FILE *f = fopen(filename, "wb");
  if (!f)
  {
    std::cerr << "FAILED (open for write)\n";
    return false;
  }

  write_u32(f, 0x12345678);
  write_f32(f, 3.14159f);
  write_u64(f, 0xABCDEF0123456789ULL);
  write_u32(f, 0xDEADBEEF);
  write_f32(f, -123.456f);

  fclose(f);

  // Read back
  f = fopen(filename, "rb");
  if (!f)
  {
    std::cerr << "FAILED (open for read)\n";
    return false;
  }

  if (read_u32(f) != 0x12345678)
  {
    std::cerr << "FAILED (u32_1)\n";
    fclose(f);
    std::remove(filename);
    return false;
  }

  float f32_val = read_f32(f);
  uint32_t f32_bits, expected_bits;
  float expected_f32 = 3.14159f;
  memcpy(&f32_bits, &f32_val, sizeof(float));
  memcpy(&expected_bits, &expected_f32, sizeof(float));
  if (f32_bits != expected_bits)
  {
    std::cerr << "FAILED (f32_1)\n";
    fclose(f);
    std::remove(filename);
    return false;
  }

  if (read_u64(f) != 0xABCDEF0123456789ULL)
  {
    std::cerr << "FAILED (u64)\n";
    fclose(f);
    std::remove(filename);
    return false;
  }

  if (read_u32(f) != 0xDEADBEEF)
  {
    std::cerr << "FAILED (u32_2)\n";
    fclose(f);
    std::remove(filename);
    return false;
  }

  fclose(f);
  std::remove(filename);

  std::cout << "PASSED\n";
  return true;
}

bool test_file_size()
{
  std::cout << "Testing file size calculations... ";

  const char *filename = "test_sizes.bin";

  FILE *f = fopen(filename, "wb");
  if (!f)
  {
    std::cerr << "FAILED (open)\n";
    return false;
  }

  write_u32(f, 1);    // 4 bytes
  write_u32(f, 2);    // 4 bytes
  write_u64(f, 3);    // 8 bytes
  write_f32(f, 4.0f); // 4 bytes

  long expected_size = 4 + 4 + 8 + 4;
  long actual_size = ftell(f);

  fclose(f);

  if (actual_size != expected_size)
  {
    std::cerr << "FAILED (expected " << expected_size << " bytes, got " << actual_size << ")\n";
    std::remove(filename);
    return false;
  }

  // Verify with file size
  f = fopen(filename, "rb");
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fclose(f);

  if (file_size != expected_size)
  {
    std::cerr << "FAILED (file size mismatch)\n";
    std::remove(filename);
    return false;
  }

  std::remove(filename);
  std::cout << "PASSED\n";
  return true;
}

int main()
{
  std::cout << "=== Binary I/O Tests ===\n\n";

  bool all_passed = true;

  all_passed &= test_endianness_detection();
  all_passed &= test_byteswap_u16();
  all_passed &= test_byteswap_u32();
  all_passed &= test_byteswap_u64();
  all_passed &= test_file_io_u32();
  all_passed &= test_file_io_u64();
  all_passed &= test_file_io_f32();
  all_passed &= test_mixed_file_io();
  all_passed &= test_file_size();

  std::cout << "\n";

  if (all_passed)
  {
    std::cout << "=== All binary I/O tests passed ✅ ===\n";
    return EXIT_SUCCESS;
  }
  else
  {
    std::cerr << "=== Some tests failed ✗ ===\n";
    return EXIT_FAILURE;
  }
}