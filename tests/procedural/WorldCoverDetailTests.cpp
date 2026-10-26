#include "json.hpp"
#include "procedural/world/WorldCoverDetail.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;

SurfaceContour rect(uint64_t id, double x, double z, double w, double h, uint32_t mat, SurfaceRole role = SurfaceRole::Ground)
{
  SurfaceContour c;
  c.id = id;
  c.loop = {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
  c.material = mat;
  c.role = role;
  return c;
}

json geometry(const WorldFinalPatches &p)
{
  json j;
  for (auto v : p.mesh.vertices)
  {
    j["vertices"].push_back({v.x, v.y, v.z});
  }
  for (auto t : p.mesh.triangles)
  {
    j["triangles"].push_back(t.vertices);
    j["materials"].push_back(t.material >= 20 ? 5 : t.material);
  }
  return j;
}

bool same(const CoverSample &a, const CoverSample &b)
{
  return a.weights == b.weights && a.owner == b.owner && a.hard == b.hard && a.variation == b.variation && a.cropRows == b.cropRows;
}

int main(int argc, char **argv)
{
  int count = 0;
  std::string error;
  auto check = [&](bool ok, std::string name) {
    if (!ok)
    {
      std::cerr << "FAIL " << name << ": " << error << '\n';
      std::exit(1);
    }
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  auto terrain = [](double x, double z) {
    return .01 * x + .02 * z;
  };
  SurfaceMapSettings map;
  map.maxX = 96;
  map.maxZ = 64;
  map.cutsX = {48};
  map.cutsZ = {32};
  std::vector<SurfaceContour> contours = {
    rect(1, 48, 0, 48, 32, 20),
    rect(2, 48, 32, 48, 32, 21),
    rect(3, 0, 0, 24, 64, 0),
    rect(4, 0, 42, 96, 8, 2, SurfaceRole::Shoulder),
    rect(5, 0, 44, 96, 4, 1, SurfaceRole::Road),
    rect(6, 68, 0, 12, 64, 3, SurfaceRole::RiverBank),
    rect(7, 71, 0, 6, 64, 9, SurfaceRole::Water),
    rect(8, 4, 6, 32, 32, 23),
    rect(9, 6, 8, 28, 28, 22),
    rect(10, 44, 8, 12, 12, 12)};
  WorldFinalPatches world;
  check(freezeWorldPatches({{1, 0, contours}}, map, {}, terrain, 16, world, &error), "freeze mixed cover fixture");
  auto before = geometry(world).dump();
  std::vector<CoverDefinition> defs = {
    {0, {0}, false, false},
    {1, {20}, false, false},
    {2, {21, 3}, false, false},
    {3, {1}, true, false},
    {4, {9}, true, false},
    {5, {22}, true, true, 1.2, .35},
    {6, {23}, true, false},
    {7, {12}, true, false},
    {8, {2}, true, false}};
  CoverDetailSettings settings;
  settings.warpMetres = 2;
  settings.transitionMetres = 4;
  WorldCoverDetail field;
  check(field.build(world, defs, settings, &error), "build normalized cover unions");
  bool normalized = true, masks = true, clipped = true, sampled = true;
  double soft = 0;
  for (double z = .125; z < 64; z += .5)
  {
    for (double x = .125; x < 96; x += .5)
    {
      CoverSample s;
      sampled &= field.sample(x, z, .25, s, &error);
      double sum = 0;
      for (double w : s.weights)
      {
        sum += w;
        normalized &= std::isfinite(w) && w >= 0 && w <= 1;
      }
      normalized &= std::abs(sum - 1) < 1e-12;
      if (s.hard)
      {
        masks &= s.weights[s.owner] == 1;
      }
      if (s.owner != 5)
      {
        clipped &= s.cropRows == 0;
      }
      if (!s.hard && s.weights[0] > .05 && s.weights[0] < .95)
      {
        soft += 1;
      }
    }
  }
  check(sampled && normalized && soft > 0, "normalized soft transitions");
  check(masks, "exact hard masks");
  check(clipped, "crop rows clipped to intended domain");
  {
    SurfaceMapSettings dense;
    dense.maxX = dense.maxZ = 64;
    for (int i = 1; i < 16; ++i)
    {
      dense.cutsX.push_back(i * 4);
      dense.cutsZ.push_back(i * 4);
    }
    WorldFinalPatches tiled;
    check(freezeWorldPatches({{1, 0, {rect(1, 0, 0, 64, 64, 0)}}}, dense, {}, terrain, 16, tiled, &error), "freeze densely partitioned cover");
    WorldCoverDetail indexed;
    check(indexed.build(tiled, {{0, {0}, true, false}}, {}, &error), "index dense cover without changing geometry");
    CoverPage page;
    check(indexed.page(0, 0, 513, 513, 1, .125, .125, page, &error), "local cover lookup admits page exceeding old global face budget");
    bool correct = true;
    for (const auto &v : page.samples)
    {
      correct &= v.owner == 0 && v.hard && v.weights == std::vector<double>{1};
    }
    check(correct, "indexed cover preserves ownership at every tile boundary and gutter");
    check(!indexed.page(0, 0, 2048, 2048, 1, .125, .125, page, &error), "cover sample allocation budget remains enforced");
  }
  CoverSample near, far, a, b;
  check(field.sample(12, 12, .01, near) && field.sample(12, 12, 4, far) && far.cropRows == .5, "crop detail converges to mean at distance");
  check(field.sample(45, 30, .2, a) && a.weights[0] > 0 && a.weights[1] > 0 && a.weights[2] > 0, "three-cover junction has normalized contributions");
  // A redundant same-cover contour changes triangulation but cannot change shading.
  auto noSplit = contours;
  noSplit.erase(noSplit.begin() + 2);
  WorldFinalPatches simpler;
  check(freezeWorldPatches({{1, 0, noSplit}}, map, {}, terrain, 16, simpler, &error), "freeze without redundant same-cover split");
  WorldCoverDetail other;
  check(other.build(simpler, defs, settings, &error), "build equivalent cover union");
  bool seamless = true;
  for (double z = 37; z < 41; z += .17)
  {
    for (double x = 20; x < 28; x += .19)
    {
      field.sample(x, z, .5, a);
      other.sample(x, z, .5, b);
      for (size_t i = 0; i < a.weights.size(); ++i)
      {
        seamless &= std::abs(a.weights[i] - b.weights[i]) < 1e-12;
      }
      seamless &= a.variation == b.variation;
    }
  }
  check(seamless, "same-cover patch seams are invisible");
  CoverPage left, right;
  check(field.page(0, 0, 97, 65, 2, .5, .5, left, &error) && field.page(96, 0, 97, 65, 2, .5, .5, right, &error), "adjacent guttered pages");
  bool gutters = true;
  for (uint32_t z = 0; z < 69; ++z)
  {
    for (uint32_t x = 0; x < 5; ++x)
    {
      gutters &= same(left.samples[z * 101 + 96 + x], right.samples[z * 101 + x]);
    }
  }
  check(gutters, "shared page and gutter samples bit identical");
  bool contacts = true;
  double nearDelta = 0, farDelta = 0;
  for (double x = 1; x < 67; x += .13)
  {
    CoverSample s, t;
    field.sample(x, 43, 8, s);
    contacts &= s.hard && s.owner == 8;
    field.sample(x, 45, 8, s);
    contacts &= s.hard && s.owner == 3;
    field.sample(x, 39, .01, s);
    field.sample(x + .04, 39, .01, t);
    nearDelta += std::abs(s.variation - t.variation);
    field.sample(x, 39, 12, s);
    field.sample(x + .04, 39, 12, t);
    farDelta += std::abs(s.variation - t.variation);
  }
  check(contacts, "road and shoulder masks survive far footprints");
  check(field.sample(5, 12, 12, a) && a.owner == 6 && a.hard && a.cropRows == 0, "crop margin remains visible at distance");
  check(field.sample(71, 30, 12, a) && a.owner == 4 && a.weights[4] == 1, "river hard mask wins exact bank boundary");
  check(field.sample(70, 30, .25, a) && a.owner == 2 && !a.hard && a.weights[4] == 0, "riverbank blends only natural covers");
  check(field.sample(50, 12, 12, a) && a.owner == 7 && a.weights[7] == 1, "building mask remains solid at distance");
  check(field.sample(0, 0, 0, a) && a.weights[0] == 1, "single-cover interior remains pure");
  double nearWeights = 0, farWeights = 0;
  for (double x = 40; x < 60; x += .13)
  {
    field.sample(x, 30, .01, a);
    field.sample(x + .04, 30, .01, b);
    for (size_t k = 0; k < a.weights.size(); ++k)
    {
      nearWeights += std::abs(a.weights[k] - b.weights[k]);
    }
    field.sample(x, 30, 12, a);
    field.sample(x + .04, 30, 12, b);
    for (size_t k = 0; k < a.weights.size(); ++k)
    {
      farWeights += std::abs(a.weights[k] - b.weights[k]);
    }
  }
  check(farWeights < nearWeights, "distant cover weights reduce subpixel motion variation");
  check(farDelta < nearDelta * .01, "subpixel camera-motion noise filtered at distance");
  std::vector<Position> scatter;
  check(field.scatter(5, 2, .4, scatter, &error) && !scatter.empty(), "clipped stable crop scatter");
  bool fit = true;
  for (auto p : scatter)
  {
    for (int i = 0; i < 32; ++i)
    {
      double angle = i * 6.283185307179586 / 32;
      field.sample(p.x + .4 * std::cos(angle), p.z + .4 * std::sin(angle), 0, a);
      fit &= a.owner == 5;
    }
  }
  check(fit, "independent scatter footprint domain check");
  std::vector<Position> repeat;
  check(field.scatter(5, 2, .4, repeat) && repeat.size() == scatter.size(), "scatter replay");
  bool identical = true;
  for (size_t i = 0; i < repeat.size(); ++i)
  {
    identical &= repeat[i].x == scatter[i].x && repeat[i].z == scatter[i].z;
  }
  check(identical, "scatter positions repeat exactly");
  settings.seed = 991;
  WorldCoverDetail changed;
  check(changed.build(world, defs, settings, &error), "alternate shading seed");
  field.sample(47, 27, .1, a);
  changed.sample(47, 27, .1, b);
  check(!same(a, b) && geometry(world).dump() == before, "shading changes without changing mesh or physical heights");
  other.sample(47, 27, .1, a);
  CoverSample oldField = a;
  auto invalid = defs;
  invalid[3].hard = false;
  check(!other.build(world, invalid, settings, &error), "soft road masks rejected");
  other.sample(47, 27, .1, a);
  check(same(a, oldField), "failed cover rebuild preserves previous field");
  CoverSample preserved = a;
  check(!field.sample(NAN, 0, 0, a, &error) && same(a, preserved), "invalid sample fails atomically");
  CoverPage saved = left;
  check(!field.page(0, 0, 2048, 2048, 16, .5, .5, left, &error) && left.samples.size() == saved.samples.size(), "page budget fails atomically");
  check(!field.scatter(5, .001, .0001, repeat, &error) && repeat.size() == scatter.size(), "scatter budget rejects without publishing");
  json validation = {{"assertions", count}, {"passed", true}, {"scatterCount", scatter.size()}, {"nearMotionVariation", nearDelta}, {"farMotionVariation", farDelta}};
  if (argc > 1)
  {
    std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    std::array<std::array<double, 3>, 9> palette = {{{.32, .48, .22}, {.51, .50, .43}, {.68, .57, .34}, {.68, .62, .49}, {.12, .42, .58}, {.62, .63, .28}, {.40, .44, .25}, {.60, .48, .40}, {.46, .43, .32}}};
    for (int variant = 0; variant < 3; ++variant)
    {
      double footprint = variant == 1 ? 4 : .25;
      auto &source = variant == 2 ? changed : field;
      CoverPage page;
      check(source.page(0, 0, 385, 257, 0, .25, footprint, page, &error), "export cover page");
      json j;
      j["phase"] = 16;
      j["mesh"] = geometry(world);
      uint32_t hardMask = 0;
      for (size_t k = 0; k < defs.size(); ++k)
      {
        if (defs[k].hard)
        {
          hardMask |= 1u << k;
        }
      }
      for (auto t : world.mesh.triangles)
      {
        size_t index = 0;
        for (; index < defs.size(); ++index)
        {
          if (std::find(defs[index].materials.begin(), defs[index].materials.end(), t.material) != defs[index].materials.end())
          {
            break;
          }
        }
        j["mesh"]["coverStyles"].push_back({palette[index][0], palette[index][1], palette[index][2], (defs[index].hard ? 1. : -1.) * double(index + 1)});
      }
      j["coverTexture"] = {{"width", 385}, {"height", 257}, {"minX", 0}, {"minZ", 0}, {"spacing", .25}, {"footprint", footprint}, {"hardMask", hardMask}};
      json pixels = json::array(), owners = json::array(), weights = json::array();
      for (const auto &s : page.samples)
      {
        std::array<double, 3> rgb{};
        for (size_t k = 0; k < defs.size(); ++k)
        {
          for (int c = 0; c < 3; ++c)
          {
            rgb[c] += s.weights[k] * palette[k][c];
          }
        }
        double variation = 1 + s.variation * .12 + (s.owner == 5 ? (s.cropRows - .5) * .14 : 0);
        for (double c : rgb)
        {
          pixels.push_back(int(std::clamp(c * variation, 0., 1.) * 255));
        }
        pixels.push_back(s.owner + 1);
        owners.push_back(s.owner);
        weights.push_back(s.weights);
      }
      j["coverTexture"]["rgba"] = pixels;
      j["owners"] = owners;
      j["weights"] = weights;
      std::ofstream(root / ("scene-" + std::to_string(variant) + "-mesh.json")) << j.dump();
    }
    validation["assertions"] = count;
    std::ofstream(root / "validation.json") << validation.dump(2);
  }
  std::cout << "PASS Step16 " << count << " assertions\n";
}
