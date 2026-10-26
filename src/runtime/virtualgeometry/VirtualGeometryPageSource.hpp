#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry
{

class VirtualGeometryPageSource;

// Where a runtime page's encoded words live outside the scene (phase 8g of
// docs/terrain-vg-unified-hierarchy-plan.md).
//
// A runtime object's page used to be a `vector<uint32_t>` the caller read out
// of wherever it kept the page and handed over at install. That copy is gone
// once the page is uploaded, so an evicted page could only come back through
// the caller - which had to keep its own asset or its own file, and its own
// request plumbing, to answer the culler. Here a page *is* a (source, offset,
// size): the scene reads it at install and reads it again after an eviction,
// on its own, and any virtual-geometry instance whose pages sit in a file can
// be served the same way.
//
// `key` and `index` are the source's own identity for the page: a chunk handle
// and a page index for the terrain scratch file, a page index for a `.vg`
// file. They let a source refuse a location that has gone stale instead of
// reading whatever now sits at `offset` - the scratch file reuses regions, and
// a stale read there is one chunk drawing another chunk's geometry.
struct VirtualGeometryPageLocation
{
  const VirtualGeometryPageSource *source = nullptr;
  uint64_t key = 0u;
  uint32_t index = 0u;
  uint64_t offset = 0u;
  uint32_t storedBytes = 0u;
  uint32_t uncompressedBytes = 0u;

  bool valid() const { return source != nullptr && storedBytes != 0u; }
};

class VirtualGeometryPageSource
{
public:
  virtual ~VirtualGeometryPageSource() = default;

  // Reads the page's encoded words - `PageBuffer::encode` output - at
  // `location`. Called on the thread that installs pages; a source that is
  // also written from a worker has to make this safe against that. Must refuse
  // a location it no longer owns rather than read through it.
  // Nonblocking sources set pending while a bounded worker reads/decompresses
  // the page. Other sources retain the synchronous API by default.
  virtual bool tryReadEncodedPage(const VirtualGeometryPageLocation &location,
      std::vector<uint32_t> &words, bool &pending, std::string *error) const
  {
    pending = false;
    return readEncodedPage(location, words, error);
  }

  virtual bool readEncodedPage(
      const VirtualGeometryPageLocation &location,
      std::vector<uint32_t> &words,
      std::string *error) const = 0;
};

} // namespace virtualgeometry
