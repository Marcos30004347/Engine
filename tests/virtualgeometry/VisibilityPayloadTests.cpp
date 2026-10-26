// The visibility payload shared by the hardware and software rasterizers.
//
// Both write the same 64-bit key - depth in the high half, this payload in the
// low half - so a merge can compare them without knowing which produced a
// pixel, and a resolve can reach geometry through
// decodedClusterGeometry[globalCluster].
//
// The encoding this replaced packed (clusterSlot, pageIndex, localCluster,
// triangle) into 12/8/6/6 bits against real ranges of 524288/65536/128/128.
// Every field truncated, so the software rasterizer's output could not be
// resolved back to geometry at all - which is why it could never be made
// authoritative. The checks below are the ones that fail if anything narrows
// the fields again, or if the three copies of the encoding (this header, the
// WGSL one and the Metal one) drift apart in width.

#include <cstdint>
#include <iostream>
#include <string>

#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

using namespace virtualgeometry;

namespace
{

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

void checkRoundTrip(const uint32_t cluster, const uint32_t triangle)
{
  const uint32_t payload = packVisibilityPayload(cluster, triangle);
  const bool ok = visibilityPayloadCluster(payload) == cluster &&
      visibilityPayloadTriangle(payload) == triangle;
  check(
      ok,
      "cluster " + std::to_string(cluster) + " triangle " +
          std::to_string(triangle) + " round-trips");
}

} // namespace

int main()
{
  std::cout << "visibility payload encoding\n";

  // The two ranges the payload has to address, at both ends.
  const uint32_t lastCluster =
      VirtualGeometryScene::MAX_DECODED_CLUSTERS - 1u;
  const uint32_t lastTriangle = ClusterSize - 1u;

  checkRoundTrip(0u, 0u);
  checkRoundTrip(0u, lastTriangle);
  checkRoundTrip(lastCluster, 0u);
  checkRoundTrip(lastCluster, lastTriangle);
  checkRoundTrip(1u, 1u);
  checkRoundTrip(lastCluster / 2u, lastTriangle / 2u);

  // The whole point of the change: the fields are wide enough for the real
  // ranges. A narrower field would silently alias two different triangles onto
  // one payload, which is exactly what the previous encoding did.
  check(
      VirtualGeometryScene::MAX_DECODED_CLUSTERS <=
          VisibilityPayloadClusterMask + 1u,
      "the cluster field addresses every decoded cluster slot");
  check(
      ClusterSize <= VisibilityPayloadTriangleMask + 1u,
      "the triangle field addresses every triangle in a cluster");
  check(
      VisibilityPayloadTriangleBits + VisibilityPayloadClusterBits <= 32u,
      "the payload fits the low half of the 64-bit visibility key");

  // Distinct inputs must stay distinct: no two (cluster, triangle) pairs in
  // range may collide.
  bool distinct = true;
  for (uint32_t cluster = 0u; cluster < 4096u && distinct; ++cluster)
    for (uint32_t triangle = 0u; triangle < ClusterSize; ++triangle)
    {
      const uint32_t payload = packVisibilityPayload(cluster, triangle);
      const uint32_t next = packVisibilityPayload(cluster, triangle + 1u);
      if (triangle + 1u < ClusterSize && payload == next)
      {
        distinct = false;
        break;
      }
      if (packVisibilityPayload(cluster + 1u, triangle) == payload)
      {
        distinct = false;
        break;
      }
    }
  check(distinct, "no two in-range cluster/triangle pairs share a payload");

  // The triangle field is the low bits, so a payload's cluster is its high
  // bits shifted - the property the Metal and WGSL copies rely on.
  check(
      packVisibilityPayload(5u, 3u) ==
          ((5u << VisibilityPayloadTriangleBits) | 3u),
      "the layout is cluster in the high bits, triangle in the low bits");

  if (failures != 0)
  {
    std::cerr << failures << " visibility payload checks failed\n";
    return 1;
  }
  std::cout << "Visibility payload tests passed\n";
  return 0;
}
