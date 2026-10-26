#include "voxel/svraster/CityVoxelSceneFormat.hpp"
#include "voxel/svraster/CityVoxelPageCompression.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <thread>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace voxel::svraster {
namespace {
constexpr uint64_t kHeaderBytes = 272;
bool fail(std::string *error, const char *message) {
  if (error) *error = message;
  return false;
}
uint64_t get64(const uint8_t *data) {
  uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i) value |= uint64_t(data[i]) << (i * 8);
  return value;
}
uint64_t checksum(const uint8_t *data, size_t count) {
  uint64_t value = 14695981039346656037ull;
  for (size_t i = 0; i != count; ++i) { value ^= data[i]; value *= 1099511628211ull; }
  return value;
}
bool readExact(std::ifstream &file, uint64_t offset, uint8_t *data, size_t count) {
  if (offset > uint64_t(std::numeric_limits<std::streamoff>::max()) ||
      count > size_t(std::numeric_limits<std::streamsize>::max())) return false;
  file.seekg(static_cast<std::streamoff>(offset));
  file.read(reinterpret_cast<char *>(data), static_cast<std::streamsize>(count));
  return bool(file);
}
std::atomic<uint64_t> nextTemporary{0};
std::string temporaryPath(const std::string &path) {
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return path + ".tmp." + std::to_string(tick) + "." +
         std::to_string(nextTemporary.fetch_add(1, std::memory_order_relaxed));
}
} // namespace

bool CityVoxelSceneFileReader::open(const std::string &path,
    const CityVoxelSceneLimits &limits, std::string *error) {
  if (error) error->clear();
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return fail(error, "Cannot open city voxel scene file");
  const auto end = file.tellg();
  if (end < 0 || uint64_t(end) < kHeaderBytes || uint64_t(end) > limits.maximumFileBytes)
    return fail(error, "City voxel scene file exceeds bounds");
  uint8_t header[kHeaderBytes]{};
  if (!readExact(file, 0, header, sizeof(header)))
    return fail(error, "Cannot read city voxel scene header");
  const uint64_t indexBytes = get64(header + 24);
  if (indexBytes < kHeaderBytes || indexBytes > limits.maximumIndexBytes ||
      indexBytes > uint64_t(end))
    return fail(error, "City voxel scene index exceeds bounds");
  std::vector<uint8_t> metadata(static_cast<size_t>(indexBytes));
  if (!readExact(file, 0, metadata.data(), metadata.size()))
    return fail(error, "Cannot read city voxel scene index");
  CityVoxelSceneIndex parsed;
  if (!deserializeCityVoxelSceneIndex(metadata.data(), indexBytes, limits, parsed, error)) return false;
  uint64_t previousEnd = indexBytes;
  for (const auto &page : parsed.pages) {
    if (!page.storedBytes || page.fileOffset < previousEnd ||
        page.fileOffset > uint64_t(end) ||
        page.storedBytes > uint64_t(end) - page.fileOffset)
      return fail(error, "City voxel page range is missing, overlapping or out of file");
    previousEnd = page.fileOffset + page.storedBytes;
  }
  path_ = path;
  limits_ = limits;
#if !defined(_WIN32)
  {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { path_.clear(); return fail(error, "Cannot open city voxel scene file"); }
    descriptor_ = std::shared_ptr<int>(new int(fd), [](int *d) { ::close(*d); delete d; });
  }
#endif
  fileBytes_ = uint64_t(end);
  indexBytes_ = indexBytes;
  index_ = std::move(parsed);
  dictionaries_.assign(size_t(CityVoxelPageKind::Dictionary), {});
  for (const auto &page : index_.pages) {
    if (page.kind != CityVoxelPageKind::Dictionary) continue;
    auto &dictionary = dictionaries_[size_t(page.assetId)];
    if (!dictionary.empty()) { path_.clear(); return fail(error, "City voxel page kind has two dictionaries"); }
    if (!readStoredPage(page.id, dictionary, error)) { path_.clear(); return false; }
  }
  return true;
}

bool CityVoxelSceneFileReader::readPage(uint32_t id, std::vector<uint8_t> &bytes,
    std::string *error) const {
  if (!readStoredPage(id, bytes, error)) return false;
  const auto &page = index_.pages[id];
  if (page.codec != CityVoxelCodec::LZ4) return true;
  if (page.kind == CityVoxelPageKind::Dictionary || size_t(page.kind) >= dictionaries_.size())
    return fail(error, "City voxel LZ4 page has no dictionary kind");
  std::vector<uint8_t> decoded;
  if (!decompressCityVoxelPage(dictionaries_[size_t(page.kind)], bytes.data(), bytes.size(),
                               page.decodedBytes, decoded, error)) return false;
  bytes = std::move(decoded);
  return true;
}

bool CityVoxelSceneFileReader::readStoredPage(uint32_t id, std::vector<uint8_t> &bytes,
    std::string *error) const {
  if (error) error->clear();
  if (path_.empty() || id >= index_.pages.size())
    return fail(error, "City voxel page ID is unavailable");
  const auto &page = index_.pages[id];
  if (!page.storedBytes || page.storedBytes > limits_.maximumStoredPageBytes ||
      page.fileOffset > fileBytes_ || page.storedBytes > fileBytes_ - page.fileOffset)
    return fail(error, "City voxel page range exceeds bounds");
  std::vector<uint8_t> candidate(page.storedBytes);
#if !defined(_WIN32)
  if (descriptor_) {
    size_t done = 0;
    while (done < candidate.size()) {
      const ssize_t got = ::pread(*descriptor_, candidate.data() + done, candidate.size() - done,
                                  off_t(page.fileOffset + done));
      if (got <= 0) return fail(error, "Cannot read city voxel page");
      done += size_t(got);
    }
  } else
#endif
  {
    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || uint64_t(file.tellg()) != fileBytes_)
      return fail(error, "City voxel scene file changed since open");
    if (!readExact(file, page.fileOffset, candidate.data(), candidate.size()))
      return fail(error, "Cannot read city voxel page");
  }
  if (checksum(candidate.data(), candidate.size()) != page.checksum)
    return fail(error, "City voxel page checksum mismatch");
  bytes = std::move(candidate);
  return true;
}

namespace {
// Optionally replaces a produced page with its stored form, setting the
// record's codec and decoded length.
using CityVoxelPageEncoder = std::function<bool(CityVoxelPageRecord &, std::vector<uint8_t> &, std::string *)>;

// Writes and verifies `temporary`; the caller publishes or removes it.
bool writeVerifiedSceneFile(const std::string &temporary, CityVoxelSceneIndex index,
    const CityVoxelSceneLimits &limits, const CityVoxelPageProducer &produce,
    const CityVoxelPageEncoder &encode, std::string *error) {
  if (error) error->clear();
  if (!produce || !validateCityVoxelSceneIndex(index, limits, error)) return false;
  for (auto &page : index.pages) {
    if (page.fileOffset || page.storedBytes || page.checksum)
      return fail(error, "City voxel writer requires uninstalled page records");
    // A compiler may specify decodedBytes for a compressed page.
    if (page.decodedBytes > limits.maximumDecodedPageBytes)
      return fail(error, "City voxel decoded page exceeds limit");
  }
  std::vector<uint8_t> metadata;
  if (!serializeCityVoxelSceneIndex(index, limits, metadata, error)) return false;
  const uint64_t indexBytes = metadata.size();
  auto cleanup = [&] { std::remove(temporary.c_str()); };
  {
    std::fstream file(temporary, std::ios::binary | std::ios::in |
                                std::ios::out | std::ios::trunc);
    if (!file) return fail(error, "Cannot create temporary city voxel scene");
    file.write(reinterpret_cast<const char *>(metadata.data()),
               static_cast<std::streamsize>(metadata.size()));
    uint64_t offset = indexBytes;
    for (uint32_t id = 0; id < index.pages.size(); ++id) {
      std::vector<uint8_t> payload;
      if (!produce(id, payload, error)) { file.close(); cleanup(); return false; }
      if (encode && !encode(index.pages[id], payload, error)) { file.close(); cleanup(); return false; }
      if (payload.empty() || payload.size() > limits.maximumStoredPageBytes ||
          offset > limits.maximumFileBytes || payload.size() > limits.maximumFileBytes - offset) {
        file.close(); cleanup();
        if (error) *error = "City voxel page " + std::to_string(id) + " (kind " +
            std::to_string(int(index.pages[id].kind)) + ", asset " + std::to_string(index.pages[id].assetId) + ") is " +
            (payload.empty() ? std::string("empty") : std::to_string(payload.size()) + " bytes") +
            " or the file exceeds its limit";
        return false;
      }
      auto &page = index.pages[id];
      if (page.codec == CityVoxelCodec::Raw &&
          page.decodedBytes && page.decodedBytes != payload.size()) {
        file.close(); cleanup(); return fail(error, "Raw city voxel page decoded length differs");
      }
      if (page.codec == CityVoxelCodec::Raw) page.decodedBytes = uint32_t(payload.size());
      if (!page.decodedBytes) {
        file.close(); cleanup(); return fail(error, "Compressed city voxel page needs decoded length");
      }
      page.fileOffset = offset;
      page.storedBytes = uint32_t(payload.size());
      page.checksum = checksum(payload.data(), payload.size());
      file.write(reinterpret_cast<const char *>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
      if (!file) { file.close(); cleanup(); return fail(error, "Cannot write city voxel page"); }
      offset += payload.size();
    }
    if (!serializeCityVoxelSceneIndex(index, limits, metadata, error) ||
        metadata.size() != indexBytes) {
      file.close(); cleanup(); return false;
    }
    file.seekp(0);
    file.write(reinterpret_cast<const char *>(metadata.data()),
               static_cast<std::streamsize>(metadata.size()));
    file.flush();
    if (!file) { file.close(); cleanup(); return fail(error, "Cannot finalize city voxel scene"); }
  }
  CityVoxelSceneFileReader verified;
  if (!verified.open(temporary, limits, error)) { cleanup(); return false; }
  for (uint32_t id = 0; id < index.pages.size(); ++id) {
    std::vector<uint8_t> page;
    if (!verified.readPage(id, page, error)) { cleanup(); return false; }
  }
  return true;
}
} // namespace

bool writeCityVoxelSceneFile(const std::string &path, CityVoxelSceneIndex index,
    const CityVoxelSceneLimits &limits, const CityVoxelPageProducer &produce,
    std::string *error, const CityVoxelSceneCompression &compression) {
  if (error) error->clear();
  const std::string temporary = temporaryPath(path);
  const auto publish = [&] {
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
      std::remove(temporary.c_str());
      return fail(error, "Cannot atomically publish city voxel scene");
    }
    return true;
  };
  if (!compression.enabled)
    return writeVerifiedSceneFile(temporary, std::move(index), limits, produce, {}, error) && publish();

  // One pass: pages come from the producer in order, a batch of one kind at
  // a time. The first batch of a kind trains its dictionary, then every
  // batch is compressed on all workers before it is written. (Writing the
  // file raw and re-reading it to compress cost a third of the export.)
  // Only raw pages are compressed; other codecs are the producer's own
  // stored form and pass through with their decoded length.
  const uint32_t sourcePages = uint32_t(index.pages.size());
  std::vector<CityVoxelPageKind> kinds;
  std::vector<uint8_t> raws;
  kinds.reserve(sourcePages);
  std::vector<uint32_t> rawCount(size_t(CityVoxelPageKind::Dictionary), 0u);
  for (const auto &page : index.pages) {
    kinds.push_back(page.kind);
    raws.push_back(page.codec == CityVoxelCodec::Raw);
    if (page.codec == CityVoxelCodec::Raw && size_t(page.kind) < rawCount.size()) ++rawCount[size_t(page.kind)];
  }
  std::vector<uint32_t> dictionaryKind;
  for (uint32_t kind = uint32_t(CityVoxelPageKind::Geometry); kind < rawCount.size(); ++kind) {
    if (rawCount[kind] < 16u) continue;
    index.pages.push_back({uint32_t(index.pages.size()), CityVoxelPageKind::Dictionary, CityVoxelCodec::Raw, kind});
    dictionaryKind.push_back(kind);
  }
  std::vector<std::unique_ptr<CityVoxelPageCompressor>> compressors(rawCount.size());
  const auto wantsDictionary = [&](CityVoxelPageKind kind) {
    return std::find(dictionaryKind.begin(), dictionaryKind.end(), uint32_t(kind)) != dictionaryKind.end();
  };

  constexpr uint32_t kBatch = 8192u;
  std::vector<std::vector<uint8_t>> batch;
  std::vector<uint32_t> decoded;
  uint32_t batchFirst = UINT32_MAX;
  const auto fillBatch = [&](uint32_t first, std::string *batchError) {
    uint32_t count = 0u;
    while (count < kBatch && first + count < sourcePages && kinds[first + count] == kinds[first]) ++count;
    batch.assign(count, {});
    decoded.assign(count, 0u);
    for (uint32_t i = 0; i < count; ++i)
      if (!produce(first + i, batch[i], batchError)) return false;
    batchFirst = first;
    const CityVoxelPageKind kind = kinds[first];
    auto &compressor = size_t(kind) < compressors.size() ? compressors[size_t(kind)] : compressors[0];
    if (wantsDictionary(kind) && !compressor) {
      std::vector<std::vector<uint8_t>> samples;
      const uint32_t step = std::max(1u, count / std::max(1u, compression.samplesPerKind));
      for (uint32_t i = 0; i < count; i += step) if (raws[first + i]) samples.push_back(batch[i]);
      auto dictionary = trainCityVoxelPageDictionary(samples, compression.dictionaryBytes);
      if (dictionary.empty()) // Nothing shared: any real bytes keep the page valid.
        for (const auto &sample : samples) if (!sample.empty()) { dictionary = sample; break; }
      if (dictionary.size() > compression.dictionaryBytes) dictionary.resize(compression.dictionaryBytes);
      compressor = std::make_unique<CityVoxelPageCompressor>(std::move(dictionary), compression.level);
    }
    if (!compressor) return true;
    std::atomic<uint32_t> next{0u};
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < std::max(1u, compression.workers); ++t)
      workers.emplace_back([&] {
        std::vector<uint8_t> packed;
        for (uint32_t i; (i = next.fetch_add(1)) < count;) {
          if (!raws[first + i] || !compressor->compress(batch[i].data(), batch[i].size(), packed)) continue;
          decoded[i] = uint32_t(batch[i].size());
          batch[i].swap(packed);
        }
      });
    for (auto &worker : workers) worker.join();
    return true;
  };
  const CityVoxelPageProducer produceStored = [&](uint32_t id, std::vector<uint8_t> &payload, std::string *pageError) {
    if (id >= sourcePages) {
      const auto &compressor = compressors[dictionaryKind[id - sourcePages]];
      if (!compressor) return fail(pageError, "City voxel dictionary was never trained");
      payload = compressor->dictionary();
      return true;
    }
    if (batchFirst == UINT32_MAX || id < batchFirst || id >= batchFirst + batch.size())
      if (!fillBatch(id, pageError)) return false;
    payload = std::move(batch[id - batchFirst]);
    return true;
  };
  const CityVoxelPageEncoder encode = [&](CityVoxelPageRecord &page, std::vector<uint8_t> &, std::string *) {
    if (page.id < sourcePages && decoded[page.id - batchFirst]) {
      page.codec = CityVoxelCodec::LZ4;
      page.decodedBytes = decoded[page.id - batchFirst];
    }
    return true;
  };
  for (auto &page : index.pages) if (page.codec == CityVoxelCodec::Raw) page.decodedBytes = 0;
  return writeVerifiedSceneFile(temporary, std::move(index), limits, produceStored, encode, error) && publish();
}

} // namespace voxel::svraster
