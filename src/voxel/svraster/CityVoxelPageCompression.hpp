#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster {

// City scene pages are a few KB each and compress poorly alone (LZ4 0.80):
// what repeats is shared between pages, file headers and quantisation
// tables and common voxel runs. A trained prefix dictionary per page kind
// carries that, so each page still decodes on its own (LZ4HC + dictionary
// measured 0.54 on VG pages).

// Picks the most widely shared segments of `samples` into at most
// `dictionaryBytes` bytes (LZ4 reads at most the last 64 KiB).
std::vector<uint8_t> trainCityVoxelPageDictionary(const std::vector<std::vector<uint8_t>> &samples,
                                                  uint32_t dictionaryBytes = 65536u);

// LZ4HC blocks against one dictionary, hashed once and attached to each
// page (reloading 64 KiB of dictionary per 5 KB page cost more than the
// compression). Thread-safe; each thread keeps its own working stream.
class CityVoxelPageCompressor {
public:
  CityVoxelPageCompressor(std::vector<uint8_t> dictionary, int level);
  ~CityVoxelPageCompressor();
  CityVoxelPageCompressor(const CityVoxelPageCompressor &) = delete;
  CityVoxelPageCompressor &operator=(const CityVoxelPageCompressor &) = delete;
  // False when the result is not smaller than the input; store it raw then.
  bool compress(const uint8_t *data, size_t size, std::vector<uint8_t> &out) const;
  const std::vector<uint8_t> &dictionary() const { return dictionary_; }

private:
  std::vector<uint8_t> dictionary_;
  void *loaded_ = nullptr; // LZ4_streamHC_t with the dictionary loaded.
  int level_;
};

// One-off form of the above.
bool compressCityVoxelPage(const std::vector<uint8_t> &dictionary, const uint8_t *data, size_t size,
                           int level, std::vector<uint8_t> &out);

bool decompressCityVoxelPage(const std::vector<uint8_t> &dictionary, const uint8_t *data, size_t size,
                             uint32_t decodedBytes, std::vector<uint8_t> &out, std::string *error = nullptr);

} // namespace voxel::svraster
