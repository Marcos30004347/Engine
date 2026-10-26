// CPU feasibility experiment for docs/virtual-geometry-hardware-raytracing-plan.md.
// This deliberately does not implement the production DAG traversal or an RHI.
// A node models one complete replacement group, not one independently replaceable
// cluster. Keep this oracle independent when implementing the GPU version.
#include "virtualgeometry/VirtualGeometryBounds.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
using virtualgeometry::AABB;
using Vec3 = std::array<float, 3>;

void require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

AABB box(Vec3 lo, Vec3 hi)
{
  AABB result;
  result.expandBy(lo.data());
  result.expandBy(hi.data());
  return result;
}

bool intersects(const AABB &a, const AABB &b)
{
  for (int i = 0; i < 3; ++i)
    if (a.maxPoint[i] < b.minPoint[i] || b.maxPoint[i] < a.minPoint[i])
      return false;
  return true;
}

// Every segment joining a receiver to any point of a finite emitter lies in
// this AABB. Intersection means "may contribute", never "definitely shadows".
AABB finiteLightVolume(const AABB &receivers, const AABB &emitter)
{
  return AABB::Union(receivers, emitter);
}

// displacement points FROM the receiver TOWARD the light. Its length must
// cover the actual shadow ray length, including any configured distance cap.
AABB directionalLightVolume(const AABB &receivers, Vec3 displacement)
{
  AABB result = receivers;
  for (int i = 0; i < 3; ++i)
  {
    result.minPoint[i] += std::min(0.0f, displacement[i]);
    result.maxPoint[i] += std::max(0.0f, displacement[i]);
  }
  return result;
}

bool contributes(const AABB &caster, const std::vector<AABB> &lightVolumes)
{
  for (const auto &volume : lightVolumes)
    if (intersects(caster, volume))
      return true;
  return false;
}

// World-space counterpart of calculateProjectedError: radial camera distance
// remains defined behind/outside the camera frustum. Not a shadow-error bound.
float cameraError(Vec3 center, float radius, float worldError, Vec3 camera,
                  float projectionScalePixels, float nearClamp)
{
  float distanceSquared = 0.0f;
  for (int i = 0; i < 3; ++i)
    distanceSquared += (center[i] - camera[i]) * (center[i] - camera[i]);
  return worldError * projectionScalePixels /
      std::max(std::sqrt(distanceSquared) - radius, nearClamp);
}

// Separate demand channels, merged after all traversals. Lexicographic classes
// cannot be inverted by many lights adding up shadow-only demand.
struct Demand
{
  uint32_t camera = 0;
  uint32_t shadow = 0;
  bool speculative = false;

  std::array<uint32_t, 2> key() const
  {
    if (camera != 0u) return {3u, camera};
    if (shadow != 0u) return {2u, shadow};
    return {speculative ? 1u : 0u, 0u};
  }
};

struct Node
{
  bool geometryReady = true;
  bool blasReady = true;
  float errorPixels = 0.0f;
  std::vector<uint32_t> children;
};

struct Cut
{
  std::vector<uint32_t> selected;
  std::vector<uint32_t> pageRequests;
  std::vector<uint32_t> blasRequests;
};

bool ready(const Node &node) { return node.geometryReady && node.blasReady; }

void request(uint32_t index, const Node &node, Cut &cut)
{
  if (!node.geometryReady) cut.pageRequests.push_back(index);
  else if (!node.blasReady) cut.blasRequests.push_back(index);
}

void selectCut(const std::vector<Node> &nodes, uint32_t index, float threshold, Cut &cut)
{
  const auto &node = nodes.at(index);
  if (!ready(node))
  {
    request(index, node, cut);
    return; // Unready root: explicitly incomplete, never invent a valid TLAS entry.
  }
  if (node.errorPixels >= threshold && !node.children.empty())
  {
    bool childrenReady = true;
    for (uint32_t child : node.children)
    {
      childrenReady &= ready(nodes.at(child));
      request(child, nodes.at(child), cut);
    }
    if (childrenReady)
    {
      for (uint32_t child : node.children) selectCut(nodes, child, threshold, cut);
      return;
    }
  }
  cut.selected.push_back(index); // Parent covers ALL siblings until replacement is ready.
}

void testLightVolumes()
{
  const AABB receivers = box({-1, -1, -11}, {1, 1, -9});
  const AABB emitter = box({10, 0, -10}, {10, 0, -10});
  const auto volume = finiteLightVolume(receivers, emitter);
  const auto blocker = box({7.9f, -0.1f, -10.1f}, {8.1f, 0.1f, -9.9f});
  // Camera at origin, looking -Z, with a 90-degree horizontal FOV. x > -z
  // is outside the right frustum; this blocker intercepts a valid shadow ray.
  const auto offscreenReceivers = box({-1, -1, -5}, {1, 1, -3});
  const auto offscreenEmitter = box({10, 0, -4}, {10, 0, -4});
  const auto offscreenBlocker = box({7.9f, -0.1f, -4.1f}, {8.1f, 0.1f, -3.9f});
  require(offscreenBlocker.minPoint[0] > -offscreenBlocker.minPoint[2], "fixture must be outside camera frustum");
  require(contributes(offscreenBlocker, {finiteLightVolume(offscreenReceivers, offscreenEmitter)}),
          "offscreen blocker on receiver-light segment must survive");
  require(intersects(blocker, volume), "blocker between receiver and light must survive");
  require(!intersects(box({11, -1, -11}, {12, 1, -9}), volume), "beyond-light caster must be rejected");
  require(!intersects(box({3, 3, -11}, {4, 4, -9}), volume), "unrelated caster must be rejected");
  require(!contributes(blocker, {}), "no lights means no shadow demand");
  const auto otherLight = box({-10, 0, -10}, {-10, 0, -10});
  require(contributes(box({-8, 0, -10}, {-7, 0, -10}),
                       {volume, finiteLightVolume(receivers, otherLight)}), "union must include the second light");

  const auto directional = directionalLightVolume(receivers, {0, 20, 0});
  require(intersects(box({0, 15, -10}, {0, 16, -10}), directional), "directional caster toward light must survive");
  require(!intersects(box({0, -4, -10}, {0, -3, -10}), directional), "caster behind receivers must be rejected");
  require(!intersects(box({0, 22, -10}, {0, 23, -10}), directional), "caster beyond shadow distance must be rejected");

  // Property experiment: sample all corner-to-corner segments of an area light
  // and receiver AABB, and every receiver corner swept toward a directional light.
  const auto area = box({8, 3, -7}, {11, 5, -6});
  const auto areaVolume = finiteLightVolume(receivers, area);
  const Vec3 sweep{-17, 23, 5};
  const auto sweptVolume = directionalLightVolume(receivers, sweep);
  for (uint32_t r = 0; r < 8; ++r)
    for (uint32_t l = 0; l < 8; ++l)
      for (uint32_t s = 0; s <= 32; ++s)
      {
        const float t = float(s) / 32.0f;
        Vec3 segment{}, swept{};
        for (int axis = 0; axis < 3; ++axis)
        {
          const float rp = ((r >> axis) & 1u) ? receivers.maxPoint[axis] : receivers.minPoint[axis];
          const float lp = ((l >> axis) & 1u) ? area.maxPoint[axis] : area.minPoint[axis];
          segment[axis] = (1.0f - t) * rp + t * lp;
          swept[axis] = rp + t * sweep[axis];
        }
        require(intersects(box(segment, segment), areaVolume), "area-light segment escaped conservative bound");
        require(intersects(box(swept, swept), sweptVolume), "directional segment escaped conservative bound");
      }
}

void testCameraError()
{
  const Vec3 camera{0, 0, 0};
  const float front = cameraError({0, 0, -10}, 1, 0.1f, camera, 1000, 0.1f);
  require(front == cameraError({10, 0, 0}, 1, 0.1f, camera, 1000, 0.1f), "offscreen error must remain radial");
  require(front == cameraError({0, 0, 10}, 1, 0.1f, camera, 1000, 0.1f), "behind-camera error must remain radial");
  require(front > cameraError({0, 0, -20}, 1, 0.1f, camera, 1000, 0.1f), "distance must reduce error");
  require(std::isfinite(cameraError(camera, 1, 0.1f, camera, 1000, 0.1f)), "camera inside bounds needs near clamp");
}

void testDemand()
{
  Demand camera{1, 0, false}, shadow{0, std::numeric_limits<uint32_t>::max(), false};
  require(camera.key() > shadow.key(), "shadow demand must not outrank visible demand");
  require(shadow.key() > Demand{0, 0, true}.key(), "real shadow demand must outrank speculation");
  const auto before = camera.key();
  for (uint32_t i = 1; i <= 1000; ++i) camera.shadow = std::max(camera.shadow, i);
  require(camera.key() == before, "shadow visits must preserve camera demand");
  require(Demand{0, 2, false}.key() > Demand{0, 1, false}.key(), "urgency must rank within shadow class");
}

void testResidentCut()
{
  std::vector<Node> nodes{{true, true, 4, {1, 2}}, {true, true, 0, {}}, {false, false, 0, {}}};
  Cut missing;
  selectCut(nodes, 0, 1, missing);
  require(missing.selected == std::vector<uint32_t>{0}, "missing sibling must retain complete parent");
  require(missing.pageRequests == std::vector<uint32_t>{2}, "request first missing page");

  nodes[2].geometryReady = true;
  Cut building;
  selectCut(nodes, 0, 1, building);
  require(building.selected == std::vector<uint32_t>{0}, "decoded child without BLAS must retain parent");
  require(building.pageRequests.empty() && building.blasRequests == std::vector<uint32_t>{2}, "resident geometry needs BLAS work only");

  nodes[2].blasReady = true;
  Cut refined;
  selectCut(nodes, 0, 1, refined);
  require(refined.selected == std::vector<uint32_t>({1, 2}), "ready replacement must omit parent");
  Cut coarse;
  selectCut(nodes, 0, 8, coarse);
  require(coarse.selected == std::vector<uint32_t>{0}, "resident descendants must not all enter TLAS");

  // A missing grandchild should retain its ready parent without hiding a ready
  // sibling elsewhere; replacement is atomic per group, not per whole object.
  nodes[1].errorPixels = 4;
  nodes[1].children = {3, 4};
  nodes.push_back({true, true, 0, {}});
  nodes.push_back({false, false, 0, {}});
  Cut nested;
  selectCut(nodes, 0, 1, nested);
  require(nested.selected == std::vector<uint32_t>({1, 2}), "nested missing page must retain local fallback");
  require(nested.pageRequests == std::vector<uint32_t>{4}, "request missing nested frontier");

  nodes[0].blasReady = false;
  Cut noRoot;
  selectCut(nodes, 0, 1, noRoot);
  require(noRoot.selected.empty() && noRoot.blasRequests == std::vector<uint32_t>{0}, "unready bootstrap must not emit invalid AS reference");
}
} // namespace

int main()
{
  try
  {
    testLightVolumes();
    testCameraError();
    testDemand();
    testResidentCut();
    std::cout << "PASS: shadow volumes (4224 sampled points), camera error, demand ordering, resident cut\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
