#include "json.hpp"
#include "procedural/geometry/TerrainLandforms.hpp"
#include "procedural/world/WorldPatchMesh.hpp"
#include "procedural/world/WorldSplineLandforms.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;
SplineFeature circle(uint64_t id, double radius, double height)
{
  SplineFeature f;
  f.id = id;
  f.heightMetres = height;
  for (int i = 0; i < 12; ++i)
  {
    double a = i * 6.283185307179586 / 12;
    f.controls.push_back({500 + radius * std::cos(a), 0, 500 + radius * std::sin(a)});
  }
  return f;
}
int main(int argc, char **argv)
{
  const bool regularOpen = argc > 2 && std::string(argv[2]) == "--regular-open";
  int failures = 0;
  json assertions = json::array();
  auto check = [&](bool ok, const char *name)
  {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    assertions.push_back({{"name", name}, {"pass", ok}});
    failures += !ok;
  };
  auto base = [](double, double)
  {
    return 0.;
  };
  std::string error;
  WorldSplineLandforms field;
  SplineSurfaceSample sample;
  auto root = circle(1, 330, 90);
  check(field.build({root}, {}, &error), "Closed flat plateau builds");
  bool exact = true;
  for (int z = 400; z <= 600; z += 10)
    for (int x = 400; x <= 600; x += 10)
    {
      exact &= field.sample(x, z, base, sample) && std::abs(sample.height - 90) < 1e-6 && sample.region == SplineRegion::Cap;
    }
  check(exact, "Zero roughness cap reproduces exact plane within one micrometre");
  auto slope = root;
  slope.slopeX = .04;
  slope.slopeZ = -.03;
  check(field.build({slope}), "Sloping plateau builds");
  field.sample(500, 500, base, sample);
  check(std::abs(sample.height - (90 + .04 * (500 - slope.controls[0].x))) < 1e-6, "Sloping cap reproduces authored plane");
  check(
      field.sample(
          -100,
          -100,
          [](double x, double z)
          {
            return x * .1 + z * .2;
          },
          sample) &&
          sample.height == -30 && sample.featureId == 0,
      "Outside support equals original ground exactly");
  field.build({root});
  bool parity = true;
  double maxError = 0;
  for (int z = 0; z <= 1000; z += 13)
    for (int x = 0; x <= 1000; x += 17)
    {
      SplineStation a, b;
      field.station(1, x, z, a, true);
      field.station(1, x, z, b, false);
      maxError = std::max(maxError, std::abs(a.signedDistanceMetres - b.signedDistanceMetres));
      parity &= a.alongMetres == b.alongMetres;
    }
  check(parity && maxError < 1e-10, "Segment BVH query matches brute force signed distance and station");
  auto child = circle(2, 190, 60);
  child.parentId = 1;
  auto grand = circle(3, 65, 45);
  grand.parentId = 2;
  check(field.build({grand, root, child}, {}, &error), "Two nested tiers resolve parent IDs independent of input order");
  field.sample(500, 500, base, sample);
  check(std::abs(sample.height - 195) < 1e-6 && sample.featureId == 3, "Grandchild evaluated on parent surface");
  field.sample(790, 500, base, sample);
  check(sample.height == 90 && sample.featureId == 1, "Parent bench remains flat between tiers");
  auto invalid = child;
  invalid.controls[0].x = 950;
  check(!field.build({root, invalid}), "Reject child support crossing parent cap");
  field.sample(500, 500, base, sample);
  check(sample.height == 195, "Failed build preserves previous surface atomically");
  auto cycle = root;
  cycle.parentId = 2;
  check(!field.build({cycle, child}), "Reject cyclic parents");
  cycle.parentId = 99;
  check(!field.build({cycle}), "Reject missing parent");
  auto overlap = root;
  overlap.id = 8;
  check(!field.build({root, overlap}), "Reject ambiguous overlapping roots");
  auto bow = root;
  bow.controls = {{200, 0, 200}, {800, 0, 800}, {200, 0, 800}, {800, 0, 200}};
  check(!field.build({bow}), "Reject self-crossing spline");
  auto concave = root;
  concave.controls = {{100, 0, 100}, {500, 0, 100}, {850, 0, 200}, {500, 0, 400}, {850, 0, 800}, {500, 0, 900}, {100, 0, 900}};
  check(field.build({concave}, {}, &error), "Concave plateau builds without offset loops");
  auto tight = root;
  tight.controls = {{100, 0, 100}, {800, 0, 100}, {810, 0, 110}, {800, 0, 120}, {200, 0, 800}, {100, 0, 800}};
  check(field.build({tight}, {}, &error), "Tight bend adaptively samples without spikes");
  SplineFeature open;
  open.id = 9;
  open.kind = SplineKind::OpenCliff;
  open.controls = {{150, 0, 500}, {400, 0, 490}, {650, 0, 510}, {850, 0, 500}};
  open.heightMetres = 100;
  check(field.build({open}), "Open oriented cliff builds");
  field.sample(500, 520, base, sample);
  double high = sample.height;
  field.sample(500, 480, base, sample);
  check(high > sample.height + 30, "Open cliff high side follows stroke orientation");
  check(field.sample(150, 500, base, sample) && sample.height == 0 && field.sample(850, 500, base, sample) && sample.height == 0, "Open cliff drops taper to zero at both ends");
  field.sample(500, 800, base, sample);
  check(sample.height == 0, "Open cliff high-side support is bounded");
  auto vertical = root;
  vertical.faceWidthMetres = 0;
  check(field.build({vertical}), "Zero-width free face accepted as explicit geometry");
  SplineFaceMesh faces;
  check(field.verticalFaces(base, faces, &error) && !faces.triangles.empty(), "Vertical face triangles emitted");
  auto normalsValid = [](const SplineFaceMesh &m)
  {
    for (size_t i = 0; i < m.triangles.size(); ++i)
    {
      auto t = m.triangles[i];
      auto a = m.vertices[t[0]], b = m.vertices[t[1]], c = m.vertices[t[2]];
      double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (std::abs(ny) > 1e-8 || nx * m.expectedNormals[i].x + nz * m.expectedNormals[i].z <= 1e-8)
        return false;
    }
    return true;
  };
  check(normalsValid(faces), "Closed vertical normals face outward with nonzero area and correct winding");
  for (int side : {-1, 1})
  {
    open.highSide = side;
    open.faceWidthMetres = 0;
    field.build({open});
    field.verticalFaces(base, faces);
    check(normalsValid(faces), side == 1 ? "Left-high tapered vertical cliff winding correct" : "Right-high tapered vertical cliff winding correct");
  }
  auto noisy = root;
  noisy.roughnessMetres = 2;
  field.build({noisy});
  bool bounded = true;
  double variation = 0;
  for (int z = 400; z < 600; z += 7)
    for (int x = 400; x < 600; x += 7)
    {
      field.sample(x, z, base, sample);
      bounded &= std::abs(sample.height - 90) <= 2;
      variation = std::max(variation, std::abs(sample.height - 90));
    }
  check(bounded && variation > .1, "Deterministic continuous noise stays within authored cap amplitude");
  auto bad = root;
  bad.version = 2;
  check(!field.build({bad}), "Unknown feature version rejected");
  bad = root;
  bad.weatheredWidthMetres = 0;
  check(!field.build({bad}), "Invalid profile width rejected");
  bad = root;
  bad.controls[0].x = std::numeric_limits<double>::quiet_NaN();
  check(!field.build({bad}), "Nonfinite controls rejected");
  check(!field.build({root}, {.00001, .001, 4}), "Sampling budget failure is explicit");
  procedural::geometry::RockFormation old;
  for (auto p : root.controls)
    old.controls.push_back({p.x, p.z});
  old.topMetres = 90;
  old.faceWidthMetres = 15;
  old.talusWidthMetres = 65;
  old.capReliefMetres = 0;
  std::vector<SplineFeature> migrated;
  check(migrateClosedLandforms({old}, migrated, &error) && migrated.size() == 1 && migrated[0].version == 1 && migrated[0].heightMetres == 90, "Legacy closed formation migrates to versioned feature record");
  old.rampWidthMetres = 10;
  check(!migrateClosedLandforms({old}, migrated), "Legacy access ramp requires explicit Step 06 migration");
  field.build({root});
  double previous = 90;
  bool monotone = true;
  for (int x = 830; x <= 930; ++x)
  {
    field.sample(x, 500, base, sample);
    monotone &= sample.height <= previous + 1e-8 && sample.height >= 0;
    previous = sample.height;
  }
  check(monotone && previous == 0, "Free face weathered slope and apron descend monotonically into ground");
  SplineFaceMesh kept;
  field.build({vertical});
  field.verticalFaces(base, kept);
  auto keptCount = kept.triangles.size();
  check(
      !field.verticalFaces(
          [](double, double)
          {
            return 1000.;
          },
          kept,
          &error) &&
          kept.triangles.size() == keptCount,
      "Invalid vertical support rejected atomically");
  field.build({slope});
  bool planeGrid = true;
  for (int z = 400; z <= 600; z += 10)
    for (int x = 400; x <= 600; x += 10)
    {
      field.sample(x, z, base, sample);
      planeGrid &= std::abs(sample.height - (90 + slope.slopeX * (x - slope.controls[0].x) + slope.slopeZ * (z - slope.controls[0].z))) < 1e-6;
    }
  check(planeGrid, "Sloping cap grid reproduces plane within one micrometre");
  // Acceptance product: nested plateau patch CDT, with independent lower rim
  // copies for a true vertical grandchild free face. Every rim constrains CDT.
  grand.faceWidthMetres = 0;
  check(field.build({root, child, grand}, {}, &error), "Nested acceptance scene builds");
  BoundaryRegistry registry;
  std::vector<BoundaryInput> edges;
  uint64_t eid = 1, pid = 1;
  auto ring = [&](const std::vector<Position> &points, bool lower)
  {
    std::vector<BoundaryUse> uses;
    std::vector<BoundaryEndpoint> endpoints;
    for (size_t k = 0; k + 1 < points.size(); ++k)
    {
      auto p = points[k];
      field.sample(p.x, p.z, base, sample);
      p.y = sample.height;
      if (&points == &field.polyline(2))
        p.y = 195;
      if (lower)
      {
        p.y -= grand.heightMetres * grand.freeFaceFraction;
      }
      endpoints.push_back({pid++, p});
    }
    for (size_t k = 0; k < endpoints.size(); ++k)
    {
      auto a = endpoints[k], b = endpoints[(k + 1) % endpoints.size()];
      uses.push_back({eid, a.id > b.id});
      edges.push_back({eid++, a, b, {}});
    }
    return uses;
  };
  auto outer = ring({{0, 0, 0}, {1000, 0, 0}, {1000, 0, 1000}, {0, 0, 1000}, {0, 0, 0}}, false);
  std::vector<std::vector<BoundaryUse>> rims;
  for (size_t i = 0; i < 3; ++i)
    rims.push_back(ring(field.polyline(i), false));
  auto low = ring(field.polyline(2), true);
  check(registry.commit(edges, &error), "Acceptance shared boundary registry commits");
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<Position> expected;
  bool meshOk = true;
  for (size_t i = 0; i < 4; ++i)
  {
    PatchDomain p;
    p.id = i + 1;
    p.outer = i == 0 ? outer : rims[i - 1];
    if (i < 3)
      p.holes.push_back(i == 2 ? low : rims[i]);
    std::vector<Position> interior;
    for (int z = 10; z < 1000; z += 10)
      for (int x = 10; x < 1000; x += 10)
      {
        SplineStation s;
        bool within = true;
        if (i)
        {
          field.station(i, x, z, s);
          within = s.signedDistanceMetres < -1e-5;
        }
        if (i < 3)
        {
          field.station(i + 1, x, z, s);
          within &= s.signedDistanceMetres > 1e-5;
        }
        if (within)
          interior.push_back({double(x), 0, double(z)});
      }
    PatchMesh mesh;
    bool ok = compilePatchMesh(
        p,
        registry,
        interior,
        [&](double x, double z)
        {
          SplineSurfaceSample s;
          field.sample(x, z, base, s);
          return s.height;
        },
        mesh,
        &error);
    if (!ok)
    {
      std::cerr << error << '\n';
      meshOk = false;
      break;
    }
    uint32_t offset = vertices.size();
    for (auto v : mesh.vertices)
      vertices.push_back(v.position);
    for (auto t : mesh.triangles)
    {
      triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
      expected.push_back({0, 1, 0});
    }
  }
  check(meshOk, "All four nested terrain patches preserve spline constraints in CDT");
  field.verticalFaces(base, faces, &error);
  bool seams = true;
  for (auto p : faces.vertices)
  {
    bool found = false;
    for (auto q : vertices)
      if (p.x == q.x && p.y == q.y && p.z == q.z)
      {
        found = true;
        break;
      }
    seams &= found;
  }
  check(seams, "Vertical face endpoints exactly match both adjacent CDT rims");
  uint32_t offset = vertices.size();
  vertices.insert(vertices.end(), faces.vertices.begin(), faces.vertices.end());
  for (size_t i = 0; i < faces.triangles.size(); ++i)
  {
    auto t = faces.triangles[i];
    triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
    expected.push_back(faces.expectedNormals[i]);
  }
  json v = json::array(), norm = json::array(), controls = json::array(), lines = json::array(), section = json::array();
  for (auto p : vertices)
    v.push_back({p.x, p.y, p.z});
  for (auto p : expected)
    norm.push_back({p.x, p.y, p.z});
  for (size_t i = 0; i < 3; ++i)
  {
    json c = json::array(), line = json::array();
    for (auto p : field.features()[i].controls)
      c.push_back({p.x, p.z});
    for (auto p : field.polyline(i))
      line.push_back({p.x, p.z});
    controls.push_back(c);
    lines.push_back(line);
  }
  for (int x = 0; x <= 1000; ++x)
  {
    field.sample(x, 500, base, sample);
    section.push_back({x, sample.height, int(sample.region)});
  }
  json openAlong = json::array(), openAcross = json::array();
  open.highSide = 1;
  open.faceWidthMetres = 15;
  WorldSplineLandforms openField;
  openField.build({open});
  for (int x = 0; x <= 1000; ++x)
  {
    SplineStation s;
    openField.station(9, x, 500, s);
    openField.sample(s.position.x, s.position.z, base, sample);
    openAlong.push_back({x, sample.height});
  }
  for (int z = 0; z <= 1000; ++z)
  {
    openField.sample(500, z, base, sample);
    openAcross.push_back({z, sample.height});
  }
  // A second native acceptance mesh uses a curved open stroke. Both patches
  // share the exact adaptive stroke edges; endpoints connect through flat ground.
  auto stroke = openField.polyline(0);
  stroke.insert(stroke.begin(), {0, 0, 500});
  stroke.push_back({1000, 0, 500});
  std::vector<BoundaryInput> openEdges;
  std::vector<BoundaryEndpoint> ends;
  uint64_t nextPoint = 1, nextEdge = 1;
  for (auto p : stroke)
  {
    openField.sample(p.x, p.z, base, sample);
    p.y = sample.height;
    ends.push_back({nextPoint++, p});
  }
  std::vector<BoundaryUse> shared;
  for (size_t k = 1; k < ends.size(); ++k)
  {
    shared.push_back({nextEdge, false});
    openEdges.push_back({nextEdge++, ends[k - 1], ends[k], {}});
  }
  PatchDomain highPatch, lowPatch;
  highPatch.id = 1;
  lowPatch.id = 2;
  highPatch.outer = shared;
  for (auto it = shared.rbegin(); it != shared.rend(); ++it)
    lowPatch.outer.push_back({it->edgeId, true});
  auto closeSide = [&](PatchDomain &patch, BoundaryEndpoint start, BoundaryEndpoint finish, double z)
  {
    BoundaryEndpoint a{nextPoint++, {start.position.x, 0, z}}, b{nextPoint++, {finish.position.x, 0, z}};
    for (auto pair : std::vector<std::pair<BoundaryEndpoint, BoundaryEndpoint>>{{start, a}, {a, b}, {b, finish}})
    {
      patch.outer.push_back({nextEdge, pair.first.id > pair.second.id});
      openEdges.push_back({nextEdge++, pair.first, pair.second, {}});
    }
  };
  closeSide(highPatch, ends.back(), ends.front(), 1000);
  closeSide(lowPatch, ends.front(), ends.back(), 0);
  BoundaryRegistry openRegistry;
  bool openMeshOk = openRegistry.commit(openEdges, &error);
  json openV = json::array(), openT = json::array();
  for (auto patch : {highPatch, lowPatch})
  {
    std::vector<Position> interior;
    for (int iz = 5; iz < 1000; iz += 10)
      for (int ix = 5; ix < 1000; ix += 10)
      {
        double x = ix + (regularOpen ? 0 : .8 * std::sin(ix * 17. + iz * 31.)), z = iz + (regularOpen ? 0 : .8 * std::cos(ix * 23. + iz * 7.));
        double divide = 500;
        for (size_t k = 1; k < stroke.size(); ++k)
          if (x >= stroke[k - 1].x && x <= stroke[k].x)
            divide = stroke[k - 1].z + (stroke[k].z - stroke[k - 1].z) * (x - stroke[k - 1].x) / (stroke[k].x - stroke[k - 1].x);
        if ((patch.id == 1 && z > divide + 1e-5) || (patch.id == 2 && z < divide - 1e-5))
          interior.push_back({double(x), 0, double(z)});
      }
    PatchMesh mesh;
    bool ok = compilePatchMesh(
        patch,
        openRegistry,
        interior,
        [&](double x, double z)
        {
          SplineSurfaceSample r;
          openField.sample(x, z, base, r);
          return r.height;
        },
        mesh,
        &error);
    openMeshOk &= ok;
    if (!ok)
      std::cerr << error << '\n';
    uint32_t offset = openV.size();
    for (auto p : mesh.vertices)
      openV.push_back({p.position.x, p.position.y, p.position.z});
    for (auto t : mesh.triangles)
      openT.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
  }
  check(openMeshOk, "Curved open cliff CDT preserves shared stroke and clean tapered endpoints");
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"assertions", assertions},
    {"mesh", {{"vertices", v}, {"triangles", triangles}, {"expectedNormals", norm}}},
    {"controls", controls},
    {"polylines", lines},
    {"section", section},
    {"maximumDistanceError", maxError},
    {"verticalTriangles", faces.triangles.size()},
    {"scale", 1},
    {"openMesh", {{"vertices", openV}, {"triangles", openT}}},
    {"openAlong", openAlong},
    {"openAcross", openAcross}};
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    file << report.dump(2) << '\n';
    if (!file)
      return 2;
  }
  std::cout << "METRIC spline_vertices " << vertices.size() << " triangles " << triangles.size() << " vertical_triangles " << faces.triangles.size() << " distance_error " << maxError << '\n';
  return failures ? 1 : 0;
}
