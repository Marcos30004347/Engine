#include "voxel/svraster/CityGroundOverlay.hpp"

#include "lz4.h"

#include <algorithm>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace voxel::svraster {
namespace {
constexpr char kMagic[8] = {'C', 'G', 'O', 'V', 'L', 'Y', '0', '1'};
constexpr uint32_t kVersion = 1;
constexpr uint64_t kMaximumRawBytes = 1ull << 30;
constexpr int64_t kBinMillimetres = 8000;

bool fail(std::string *error, const std::string &message) {
  if (error) *error = message;
  return false;
}
uint64_t fnv(const uint8_t *data, size_t size) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) { hash ^= data[i]; hash *= 1099511628211ull; }
  return hash;
}

class Writer {
public:
  template <typename T> void put(T value) {
    for (unsigned i = 0; i < sizeof(T); ++i)
      bytes.push_back(uint8_t(uint64_t(value) >> (8u * i)));
  }
  void raw(const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    bytes.insert(bytes.end(), p, p + size);
  }
  std::vector<uint8_t> bytes;
};

class Reader {
public:
  Reader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
  template <typename T> bool get(T &value) {
    if (bytes_.size() - at_ < sizeof(T)) return false;
    uint64_t result = 0;
    for (unsigned i = 0; i < sizeof(T); ++i) result |= uint64_t(bytes_[at_ + i]) << (8u * i);
    at_ += sizeof(T);
    value = T(result);
    return true;
  }
  bool raw(void *data, size_t size) {
    if (bytes_.size() - at_ < size) return false;
    std::memcpy(data, bytes_.data() + at_, size);
    at_ += size;
    return true;
  }
  size_t remaining() const { return bytes_.size() - at_; }

private:
  const std::vector<uint8_t> &bytes_;
  size_t at_ = 0;
};

bool inside(const CityGroundPrism &prism, double x, double z) {
  bool in = false;
  for (const auto &ring : prism.rings)
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
      const double xi = ring[i][0], zi = ring[i][1], xj = ring[j][0], zj = ring[j][1];
      if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
    }
  return in;
}
} // namespace

bool CityGroundOverlay::load(CityGroundOverlayData data, std::string *error) {
  if (error) error->clear();
  const uint64_t groundCount = uint64_t(data.groundWidth) * data.groundHeight;
  const uint64_t paintCount = uint64_t(data.paintWidth) * data.paintHeight;
  if (!data.groundSpacingMillimetres || data.groundWidth < 2 || data.groundHeight < 2 ||
      data.groundMillimetres.size() != groundCount || data.groundWeight.size() != groundCount)
    return fail(error, "City ground overlay grid is invalid");
  if (paintCount && (!data.paintSpacingMillimetres || data.paint.size() != paintCount))
    return fail(error, "City ground overlay paint grid is invalid");
  std::array<int32_t, 4> total{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
  prismBounds_.clear();
  for (const auto &prism : data.prisms) {
    if (prism.rings.empty() || prism.rings[0].size() < 3 ||
        prism.topMillimetres < prism.bottomMillimetres)
      return fail(error, "City ground overlay prism is invalid");
    std::array<int32_t, 4> box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const auto &ring : prism.rings) {
      if (ring.size() < 3) return fail(error, "City ground overlay ring is degenerate");
      for (const auto &p : ring) {
        box[0] = std::min(box[0], p[0]); box[1] = std::min(box[1], p[1]);
        box[2] = std::max(box[2], p[0]); box[3] = std::max(box[3], p[1]);
      }
    }
    prismBounds_.push_back(box);
    total = {std::min(total[0], box[0]), std::min(total[1], box[1]),
             std::max(total[2], box[2]), std::max(total[3], box[3])};
  }
  binFirst_.clear(); binPrisms_.clear(); binsX_ = binsZ_ = 0;
  if (!data.prisms.empty()) {
    const auto bin = [](int64_t v) { return v >= 0 ? v / kBinMillimetres : -((-v + kBinMillimetres - 1) / kBinMillimetres); };
    binMinimumX_ = bin(total[0]); binMinimumZ_ = bin(total[1]);
    const int64_t width = bin(total[2]) - binMinimumX_ + 1, height = bin(total[3]) - binMinimumZ_ + 1;
    if (width * height > (1ll << 24)) return fail(error, "City ground overlay is too large to index");
    binsX_ = uint32_t(width); binsZ_ = uint32_t(height);
    std::vector<uint32_t> counts(size_t(width * height) + 1, 0);
    const auto each = [&](uint32_t i, auto &&visit) {
      const auto &b = prismBounds_[i];
      for (int64_t z = bin(b[1]); z <= bin(b[3]); ++z)
        for (int64_t x = bin(b[0]); x <= bin(b[2]); ++x)
          visit(size_t((z - binMinimumZ_) * width + (x - binMinimumX_)));
    };
    for (uint32_t i = 0; i < data.prisms.size(); ++i) each(i, [&](size_t b) { ++counts[b + 1]; });
    for (size_t i = 1; i < counts.size(); ++i) counts[i] += counts[i - 1];
    binFirst_ = counts;
    binPrisms_.resize(counts.back());
    for (uint32_t i = 0; i < data.prisms.size(); ++i) each(i, [&](size_t b) { binPrisms_[counts[b]++] = i; });
  }
  data_ = std::move(data);
  return true;
}

double CityGroundOverlay::ground(double x, double z, double &weight) const {
  const double spacing = double(data_.groundSpacingMillimetres);
  const double gx = (x * 1000.0 - double(data_.originMillimetres[0])) / spacing;
  const double gz = (z * 1000.0 - double(data_.originMillimetres[1])) / spacing;
  weight = 0.0;
  if (!(gx >= 0.0 && gz >= 0.0 && gx <= data_.groundWidth - 1 && gz <= data_.groundHeight - 1)) return 0.0;
  const uint32_t x0 = std::min(uint32_t(gx), data_.groundWidth - 2);
  const uint32_t z0 = std::min(uint32_t(gz), data_.groundHeight - 2);
  const double fx = gx - x0, fz = gz - z0;
  const auto at = [&](uint32_t ix, uint32_t iz) { return size_t(iz) * data_.groundWidth + ix; };
  const size_t a = at(x0, z0), b = at(x0 + 1, z0), c = at(x0, z0 + 1), d = at(x0 + 1, z0 + 1);
  const auto mix = [&](double va, double vb, double vc, double vd) {
    return (va * (1 - fx) + vb * fx) * (1 - fz) + (vc * (1 - fx) + vd * fx) * fz;
  };
  weight = mix(data_.groundWeight[a], data_.groundWeight[b], data_.groundWeight[c],
               data_.groundWeight[d]) / 255.0;
  return mix(data_.groundMillimetres[a], data_.groundMillimetres[b], data_.groundMillimetres[c],
             data_.groundMillimetres[d]) * 0.001;
}

CityGroundColumn CityGroundOverlay::sample(double x, double z) const {
  CityGroundColumn column;
  if (empty()) return column;
  column.heightMetres = ground(x, z, column.weight);
  if (column.weight <= 0.0) return column;
  if (!data_.paint.empty()) {
    const int64_t px = int64_t(std::floor((x * 1000.0 - double(data_.originMillimetres[0])) /
                                          data_.paintSpacingMillimetres));
    const int64_t pz = int64_t(std::floor((z * 1000.0 - double(data_.originMillimetres[1])) /
                                          data_.paintSpacingMillimetres));
    if (px >= 0 && pz >= 0 && px < data_.paintWidth && pz < data_.paintHeight)
      column.material = data_.paint[size_t(pz) * data_.paintWidth + size_t(px)];
  }
  if (binsX_ == 0) return column;
  const double mx = x * 1000.0, mz = z * 1000.0;
  const int64_t bx = int64_t(std::floor(mx / kBinMillimetres)) - binMinimumX_;
  const int64_t bz = int64_t(std::floor(mz / kBinMillimetres)) - binMinimumZ_;
  if (bx < 0 || bz < 0 || bx >= binsX_ || bz >= binsZ_) return column;
  const size_t binIndex = size_t(bz) * binsX_ + size_t(bx);
  double best = -std::numeric_limits<double>::infinity();
  const CityGroundPrism *top = nullptr;
  for (uint32_t i = binFirst_[binIndex]; i < binFirst_[binIndex + 1]; ++i) {
    const uint32_t index = binPrisms_[i];
    const auto &box = prismBounds_[index];
    if (mx < box[0] || mx > box[2] || mz < box[1] || mz > box[3]) continue;
    const auto &prism = data_.prisms[index];
    if (!inside(prism, mx, mz)) continue;
    const double height = prism.topMillimetres * 0.001 +
        (prism.terrainFollowing ? column.heightMetres : 0.0);
    if (height > best) { best = height; top = &prism; }
  }
  if (top) {
    column.heightMetres = best;
    column.material = top->topMaterial;
    column.paved = true;
  }
  return column;
}

void CityGroundOverlay::packGPU(double heightOffsetMetres, std::vector<uint32_t> &words) const {
  // Header (32 words), then the sections it points at. Must match
  // city_ground_overlay.wgsl.
  words.assign(32u, 0u);
  if (empty()) return;
  const auto f = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; };
  const auto i = [](int64_t v) { return uint32_t(int32_t(v)); };
  const auto bytes = [&](const std::vector<uint8_t> &values) {
    const uint32_t first = uint32_t(words.size());
    words.resize(words.size() + (values.size() + 3) / 4, 0u);
    for (size_t k = 0; k < values.size(); ++k) words[first + k / 4] |= uint32_t(values[k]) << (8u * (k % 4));
    return first;
  };
  words[0] = 1u;
  words[1] = data_.groundWidth; words[2] = data_.groundHeight; words[3] = data_.groundSpacingMillimetres;
  words[4] = i(data_.originMillimetres[0]); words[5] = i(data_.originMillimetres[1]);
  words[6] = f(float(double(data_.siteOriginMillimetres[0]) * 0.001));
  words[7] = f(float(double(data_.siteOriginMillimetres[1]) * 0.001));
  words[8] = f(float(heightOffsetMetres));
  words[9] = data_.paint.empty() ? 0u : data_.paintWidth;
  words[10] = data_.paint.empty() ? 0u : data_.paintHeight;
  words[11] = data_.paintSpacingMillimetres;
  words[12] = i(binMinimumX_); words[13] = i(binMinimumZ_); words[14] = binsX_; words[15] = binsZ_;
  words[16] = uint32_t(kBinMillimetres);
  words[17] = uint32_t(words.size());
  for (const int32_t g : data_.groundMillimetres) words.push_back(uint32_t(g));
  words[18] = bytes(data_.groundWeight);
  words[19] = bytes(data_.paint);
  words[20] = uint32_t(words.size());
  words.insert(words.end(), binFirst_.begin(), binFirst_.end());
  words[21] = uint32_t(words.size());
  words.insert(words.end(), binPrisms_.begin(), binPrisms_.end());
  // Prisms: bounds (4), top mm, flags (bit 0 terrain following, bits 8-15
  // top material), first ring, ring count.
  words[22] = uint32_t(words.size());
  words[25] = uint32_t(data_.prisms.size());
  const size_t prismWords = words.size();
  words.resize(prismWords + data_.prisms.size() * 8u, 0u);
  std::vector<uint32_t> rings, vertices;
  for (size_t p = 0; p < data_.prisms.size(); ++p) {
    const auto &prism = data_.prisms[p];
    uint32_t *record = words.data() + prismWords + p * 8u;
    for (unsigned k = 0; k < 4; ++k) record[k] = i(prismBounds_[p][k]);
    record[4] = i(prism.topMillimetres);
    record[5] = (prism.terrainFollowing ? 1u : 0u) | (uint32_t(prism.topMaterial) << 8u);
    record[6] = uint32_t(rings.size() / 2u);
    record[7] = uint32_t(prism.rings.size());
    for (const auto &ring : prism.rings) {
      rings.push_back(uint32_t(vertices.size() / 2u));
      rings.push_back(uint32_t(ring.size()));
      for (const auto &v : ring) { vertices.push_back(i(v[0])); vertices.push_back(i(v[1])); }
    }
  }
  words[23] = uint32_t(words.size());
  words.insert(words.end(), rings.begin(), rings.end());
  words[24] = uint32_t(words.size());
  words.insert(words.end(), vertices.begin(), vertices.end());
}

bool rasterizeCityGroundMaterials(const CityGroundOverlay &overlay, double texelMetres,
                                  uint32_t workers, CityGroundMaterialGrid &grid, std::string *error) {
  const auto &data = overlay.data();
  if (overlay.empty() || !(texelMetres > 0.0)) return fail(error, "City ground material grid needs an overlay");
  CityGroundMaterialGrid next;
  next.texelMetres = texelMetres;
  next.originMetres = {double(data.originMillimetres[0]) * 0.001, double(data.originMillimetres[1]) * 0.001};
  const double spanX = double(data.groundWidth - 1) * data.groundSpacingMillimetres * 0.001;
  const double spanZ = double(data.groundHeight - 1) * data.groundSpacingMillimetres * 0.001;
  next.width = uint32_t(spanX / texelMetres);
  next.height = uint32_t(spanZ / texelMetres);
  if (!next.width || !next.height || uint64_t(next.width) * next.height > (1ull << 28))
    return fail(error, "City ground material grid is empty or too large");
  next.materials.assign(size_t(next.width) * next.height, 255u);
  std::atomic<uint32_t> row{0};
  std::vector<std::thread> threads;
  for (uint32_t t = 0; t < std::max(1u, workers); ++t)
    threads.emplace_back([&] {
      for (uint32_t z; (z = row.fetch_add(1)) < next.height;)
        for (uint32_t x = 0; x < next.width; ++x) {
          const auto column = overlay.sample(next.originMetres[0] + (x + 0.5) * texelMetres,
                                             next.originMetres[1] + (z + 0.5) * texelMetres);
          if (column.weight >= 0.5 && column.material != 255)
            next.materials[size_t(z) * next.width + x] = column.material;
        }
    });
  for (auto &thread : threads) thread.join();
  grid = std::move(next);
  return true;
}

bool writeCityGroundOverlay(const std::string &path, const CityGroundOverlayData &data,
                            std::string *error) {
  CityGroundOverlay check;
  if (!check.load(data, error)) return false;
  Writer w;
  w.put<int64_t>(data.siteOriginMillimetres[0]); w.put<int64_t>(data.siteOriginMillimetres[1]);
  w.put<int64_t>(data.originMillimetres[0]); w.put<int64_t>(data.originMillimetres[1]);
  w.put<uint32_t>(data.groundSpacingMillimetres);
  w.put<uint32_t>(data.groundWidth); w.put<uint32_t>(data.groundHeight);
  for (int32_t v : data.groundMillimetres) w.put<int32_t>(v);
  w.raw(data.groundWeight.data(), data.groundWeight.size());
  w.put<uint32_t>(data.paintSpacingMillimetres);
  w.put<uint32_t>(data.paintWidth); w.put<uint32_t>(data.paintHeight);
  w.raw(data.paint.data(), data.paint.size());
  w.put<uint32_t>(uint32_t(data.prisms.size()));
  for (const auto &prism : data.prisms) {
    w.put<int32_t>(prism.bottomMillimetres); w.put<int32_t>(prism.topMillimetres);
    w.put<uint8_t>(prism.terrainFollowing ? 1 : 0);
    w.put<uint8_t>(prism.topMaterial); w.put<uint8_t>(prism.sideMaterial);
    w.put<uint32_t>(uint32_t(prism.rings.size()));
    for (const auto &ring : prism.rings) {
      w.put<uint32_t>(uint32_t(ring.size()));
      for (const auto &p : ring) { w.put<int32_t>(p[0]); w.put<int32_t>(p[1]); }
    }
  }
  if (w.bytes.size() > kMaximumRawBytes || w.bytes.size() > size_t(LZ4_MAX_INPUT_SIZE))
    return fail(error, "City ground overlay is too large");
  std::vector<uint8_t> packed(size_t(LZ4_compressBound(int(w.bytes.size()))));
  const int packedSize = LZ4_compress_default(reinterpret_cast<const char *>(w.bytes.data()),
      reinterpret_cast<char *>(packed.data()), int(w.bytes.size()), int(packed.size()));
  if (packedSize <= 0) return fail(error, "City ground overlay compression failed");
  packed.resize(size_t(packedSize));
  Writer header;
  header.raw(kMagic, sizeof(kMagic));
  header.put<uint32_t>(kVersion);
  header.put<uint64_t>(w.bytes.size());
  header.put<uint64_t>(fnv(w.bytes.data(), w.bytes.size()));
  header.put<uint64_t>(packed.size());
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(header.bytes.data()), std::streamsize(header.bytes.size()));
    out.write(reinterpret_cast<const char *>(packed.data()), std::streamsize(packed.size()));
    if (!out) return fail(error, "City ground overlay write failed: " + temporary);
  }
  std::error_code renameError;
  std::filesystem::rename(temporary, path, renameError);
  if (renameError) return fail(error, "City ground overlay publish failed: " + renameError.message());
  return true;
}

bool readCityGroundOverlay(const std::string &path, CityGroundOverlayData &result,
                           std::string *error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(error, "Cannot open city ground overlay: " + path);
  std::vector<uint8_t> header(sizeof(kMagic) + 4 + 8 * 3);
  if (!in.read(reinterpret_cast<char *>(header.data()), std::streamsize(header.size())) ||
      std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0)
    return fail(error, "Not a city ground overlay: " + path);
  Reader h(header);
  char magic[8]; uint32_t version = 0; uint64_t rawSize = 0, checksum = 0, packedSize = 0;
  h.raw(magic, 8); h.get(version); h.get(rawSize); h.get(checksum); h.get(packedSize);
  if (version != kVersion) return fail(error, "Unsupported city ground overlay version");
  if (!rawSize || rawSize > kMaximumRawBytes || !packedSize ||
      packedSize > uint64_t(LZ4_compressBound(int(rawSize))))
    return fail(error, "City ground overlay sizes are invalid");
  std::vector<uint8_t> packed(packedSize), raw(rawSize);
  if (!in.read(reinterpret_cast<char *>(packed.data()), std::streamsize(packed.size())))
    return fail(error, "City ground overlay is truncated");
  if (LZ4_decompress_safe(reinterpret_cast<const char *>(packed.data()),
                          reinterpret_cast<char *>(raw.data()), int(packed.size()),
                          int(raw.size())) != int(raw.size()) ||
      fnv(raw.data(), raw.size()) != checksum)
    return fail(error, "City ground overlay payload is corrupt");
  Reader r(raw);
  CityGroundOverlayData data;
  const auto bad = [&] { return fail(error, "City ground overlay payload is malformed"); };
  if (!r.get(data.siteOriginMillimetres[0]) || !r.get(data.siteOriginMillimetres[1]) ||
      !r.get(data.originMillimetres[0]) || !r.get(data.originMillimetres[1]) ||
      !r.get(data.groundSpacingMillimetres) || !r.get(data.groundWidth) || !r.get(data.groundHeight))
    return bad();
  const uint64_t groundCount = uint64_t(data.groundWidth) * data.groundHeight;
  if (groundCount * 5 > r.remaining()) return bad();
  data.groundMillimetres.resize(groundCount);
  for (auto &v : data.groundMillimetres) r.get(v);
  data.groundWeight.resize(groundCount);
  r.raw(data.groundWeight.data(), groundCount);
  if (!r.get(data.paintSpacingMillimetres) || !r.get(data.paintWidth) || !r.get(data.paintHeight))
    return bad();
  const uint64_t paintCount = uint64_t(data.paintWidth) * data.paintHeight;
  if (paintCount > r.remaining()) return bad();
  data.paint.resize(paintCount);
  r.raw(data.paint.data(), paintCount);
  uint32_t prismCount = 0;
  if (!r.get(prismCount) || uint64_t(prismCount) * 15 > r.remaining()) return bad();
  data.prisms.resize(prismCount);
  for (auto &prism : data.prisms) {
    uint8_t flags = 0; uint32_t rings = 0;
    if (!r.get(prism.bottomMillimetres) || !r.get(prism.topMillimetres) || !r.get(flags) ||
        !r.get(prism.topMaterial) || !r.get(prism.sideMaterial) || !r.get(rings) ||
        uint64_t(rings) * 4 > r.remaining())
      return bad();
    prism.terrainFollowing = flags & 1u;
    prism.rings.resize(rings);
    for (auto &ring : prism.rings) {
      uint32_t points = 0;
      if (!r.get(points) || uint64_t(points) * 8 > r.remaining()) return bad();
      ring.resize(points);
      for (auto &p : ring) { r.get(p[0]); r.get(p[1]); }
    }
  }
  if (r.remaining()) return bad();
  CityGroundOverlay check;
  if (!check.load(data, error)) return false;
  result = std::move(data);
  return true;
}

} // namespace voxel::svraster
