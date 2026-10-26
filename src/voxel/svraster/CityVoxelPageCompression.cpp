#include "voxel/svraster/CityVoxelPageCompression.hpp"

#include "lz4.h"
#include "lz4hc.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <queue>
#include <tuple>

namespace voxel::svraster {
namespace {
constexpr uint32_t kKmer = 8u;        // Bytes per counted substring.
constexpr uint32_t kSegment = 64u;    // Bytes per dictionary piece.
constexpr uint32_t kStride = 32u;     // Candidate segment spacing.
constexpr uint32_t kTableBits = 22u;  // Shared-count table; collisions only blur scores.

uint32_t kmerHash(const uint8_t *p) {
  uint64_t value;
  std::memcpy(&value, p, sizeof(value));
  return uint32_t((value * 0x9E3779B97F4A7C15ull) >> (64u - kTableBits));
}

struct StreamDeleter { void operator()(LZ4_streamHC_t *s) const { LZ4_freeStreamHC(s); } };
} // namespace

std::vector<uint8_t> trainCityVoxelPageDictionary(const std::vector<std::vector<uint8_t>> &samples,
                                                  uint32_t dictionaryBytes) {
  dictionaryBytes = std::min<uint32_t>(dictionaryBytes, 65536u);
  // How many samples contain each substring: a piece is worth keeping when
  // many pages could refer to it, not when one page repeats it.
  std::vector<uint32_t> shared(size_t(1) << kTableBits, 0u);
  std::vector<uint32_t> hashes;
  for (const auto &sample : samples) {
    if (sample.size() < kKmer) continue;
    hashes.clear();
    for (size_t i = 0; i + kKmer <= sample.size(); ++i) hashes.push_back(kmerHash(sample.data() + i));
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    for (const uint32_t h : hashes) ++shared[h];
  }
  const auto score = [&](uint32_t sample, uint32_t at) {
    const auto &bytes = samples[sample];
    uint64_t total = 0;
    for (uint32_t i = 0; i + kKmer <= kSegment; ++i) {
      const uint32_t count = shared[kmerHash(bytes.data() + at + i)];
      if (count > 1u) total += count - 1u;
    }
    return total;
  };
  using Candidate = std::tuple<uint64_t, uint32_t, uint32_t>; // score, sample, offset
  std::priority_queue<Candidate> heap;
  for (uint32_t s = 0; s < samples.size(); ++s)
    for (uint32_t at = 0; at + kSegment <= samples[s].size(); at += kStride)
      if (const uint64_t value = score(s, at)) heap.push({value, s, at});
  std::vector<std::vector<uint8_t>> chosen;
  uint32_t used = 0;
  while (!heap.empty() && used + kSegment <= dictionaryBytes) {
    auto [value, s, at] = heap.top();
    heap.pop();
    // Lazy greedy: what is already in the dictionary no longer counts.
    const uint64_t current = score(s, at);
    if (!current) continue;
    if (!heap.empty() && current < std::get<0>(heap.top())) {
      heap.push({current, s, at});
      continue;
    }
    const uint8_t *piece = samples[s].data() + at;
    chosen.emplace_back(piece, piece + kSegment);
    for (uint32_t i = 0; i + kKmer <= kSegment; ++i) shared[kmerHash(piece + i)] = 0u;
    used += kSegment;
  }
  // LZ4 matches any offset within the window equally; most valuable last
  // only keeps them inside it if a caller trims the front.
  std::vector<uint8_t> dictionary;
  dictionary.reserve(used);
  for (auto it = chosen.rbegin(); it != chosen.rend(); ++it) dictionary.insert(dictionary.end(), it->begin(), it->end());
  return dictionary;
}

CityVoxelPageCompressor::CityVoxelPageCompressor(std::vector<uint8_t> dictionary, int level)
    : dictionary_(std::move(dictionary)), level_(level) {
  if (dictionary_.empty()) return;
  auto *stream = LZ4_createStreamHC();
  if (!stream) return;
  LZ4_resetStreamHC_fast(stream, level_);
  LZ4_loadDictHC(stream, reinterpret_cast<const char *>(dictionary_.data()), int(dictionary_.size()));
  loaded_ = stream;
}

CityVoxelPageCompressor::~CityVoxelPageCompressor() {
  if (loaded_) LZ4_freeStreamHC(static_cast<LZ4_streamHC_t *>(loaded_));
}

bool CityVoxelPageCompressor::compress(const uint8_t *data, size_t size, std::vector<uint8_t> &out) const {
  if (!size || size > size_t(LZ4_MAX_INPUT_SIZE)) return false;
  thread_local std::unique_ptr<LZ4_streamHC_t, StreamDeleter> stream(LZ4_createStreamHC());
  if (!stream) return false;
  LZ4_resetStreamHC_fast(stream.get(), level_);
  LZ4_attach_HC_dictionary(stream.get(), static_cast<const LZ4_streamHC_t *>(loaded_));
  out.resize(size_t(LZ4_compressBound(int(size))));
  const int written = LZ4_compress_HC_continue(stream.get(), reinterpret_cast<const char *>(data),
                                               reinterpret_cast<char *>(out.data()), int(size), int(out.size()));
  if (written <= 0 || size_t(written) >= size) return false;
  out.resize(size_t(written));
  return true;
}

bool compressCityVoxelPage(const std::vector<uint8_t> &dictionary, const uint8_t *data, size_t size,
                           int level, std::vector<uint8_t> &out) {
  return CityVoxelPageCompressor(dictionary, level).compress(data, size, out);
}

bool decompressCityVoxelPage(const std::vector<uint8_t> &dictionary, const uint8_t *data, size_t size,
                             uint32_t decodedBytes, std::vector<uint8_t> &out, std::string *error) {
  out.resize(decodedBytes);
  const int read = LZ4_decompress_safe_usingDict(
      reinterpret_cast<const char *>(data), reinterpret_cast<char *>(out.data()), int(size), int(decodedBytes),
      reinterpret_cast<const char *>(dictionary.data()), int(dictionary.size()));
  if (read != int(decodedBytes)) {
    if (error) *error = "City voxel page does not decode to its recorded length";
    return false;
  }
  return true;
}

} // namespace voxel::svraster
