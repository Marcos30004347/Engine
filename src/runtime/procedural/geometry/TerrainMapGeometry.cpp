#include "procedural/geometry/TerrainMapSource.hpp"
#include "procedural/geometry/TerrainRegionContours.hpp"
#include <memory>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <queue>
#include <vector>

namespace procedural::geometry
{
namespace
{
double smooth(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * t * (t * (t * 6 - 15) + 10); }
uint64_t mix(uint64_t n)
{
  n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ull; n ^= n >> 27;
  n *= 0x94d049bb133111ebull; return n ^ (n >> 31);
}
double noise(Point2 p, double wavelength, uint64_t seed)
{
  p.x /= wavelength; p.y /= wavelength;
  const auto x = int64_t(std::floor(p.x)), y = int64_t(std::floor(p.y));
  const double tx = p.x - x, ty = p.y - y;
  const auto corner = [&](int64_t a, int64_t b)
  {
    const auto h = mix(seed ^ mix(uint64_t(a)) ^ (mix(uint64_t(b)) << 1));
    const double angle = double(h & 65535) * (6.283185307179586 / 65536.0);
    return std::cos(angle) * (p.x - a) + std::sin(angle) * (p.y - b);
  };
  const double sx = smooth(tx), sy = smooth(ty);
  return ((1 - sx) * corner(x, y) + sx * corner(x + 1, y)) * (1 - sy) +
      ((1 - sx) * corner(x, y + 1) + sx * corner(x + 1, y + 1)) * sy;
}
bool contains(const std::vector<Point2> &polygon, Point2 p)
{
  if (polygon.size() < 3) return false;
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
  {
    const auto &a = polygon[i], &b = polygon[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (b.x - a.x) * (p.y - a.y) / (b.y - a.y)) inside = !inside;
  }
  return inside;
}
double segmentDistance(Point2 p, Point2 a, Point2 b)
{
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / std::max(1e-12, dx * dx + dy * dy), 0.0, 1.0);
  return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}
}
// Fractal displacement of the region lookup. Land, water and biome are all
// decided per map-graph cell, so the coastline and every biome boundary is a
// map-graph cell boundary: a 700 m polygon edge, straight. Warping the query
// point before the cell is resolved gives the boundary detail at every scale
// below the cell while leaving the coarse shape - which island, which sea -
// exactly where the map put it. That is "finer cells controlled by the high
// level cell" without a second partition to keep in step.
//
// Everything that reads the map goes through regionNode, so water, biome and
// landform stay consistent with each other for free. The cost is two hashes per
// octave on a hot path, so it stays at three octaves.
namespace
{
double valueNoise(const double x, const double y, const uint64_t salt)
{
  const auto hash = [salt](const int64_t a, const int64_t b)
  {
    uint64_t n = uint64_t(a) * 0x9E3779B97F4A7C15ull ^ uint64_t(b) * 0xC2B2AE3D27D4EB4Full ^ salt;
    n ^= n >> 29; n *= 0xBF58476D1CE4E5B9ull; n ^= n >> 32;
    return double(n >> 11) / double(1ull << 53);
  };
  const double fx = std::floor(x), fy = std::floor(y);
  const auto ix = int64_t(fx), iy = int64_t(fy);
  const double tx = x - fx, ty = y - fy;
  const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
  const double a = hash(ix, iy), b = hash(ix + 1, iy);
  const double c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
  return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}
}
// The largest displacement the warp can produce on one axis: the octave sum.
// The bucket index is dilated by exactly this, so the uniform-bucket shortcut
// stays conservative without dilating further than it has to - every extra
// metre of dilation makes more buckets non-uniform and pushes more queries onto
// the point-in-polygon path.
double TerrainMapSource::regionWarpReach() const
{
  double reach = 0, amplitude = regionWarpMetres;
  for (uint32_t octave = 0; octave < kRegionWarpOctaves; ++octave) { reach += amplitude; amplitude *= .5; }
  return reach;
}
Point2 TerrainMapSource::regionWarp(Point2 p) const
{
  if (!(regionWarpMetres > 0)) return {0, 0};
  double dx = 0, dy = 0, amplitude = regionWarpMetres, frequency = 1.0 / std::max(1.0, regionWarpScaleMetres);
  for (uint32_t octave = 0; octave < kRegionWarpOctaves; ++octave)
  {
    dx += (valueNoise(p.x * frequency, p.y * frequency, 0x51A1u + octave) - .5) * 2.0 * amplitude;
    dy += (valueNoise(p.x * frequency, p.y * frequency, 0x9E11u + octave) - .5) * 2.0 * amplitude;
    amplitude *= .5; frequency *= 2.07;
  }
  // Fade to nothing at the world rim so the border stays a square.
  const double margin = regionWarpMetres * 4.0;
  const double edge = std::min(std::min(p.x, extentMetres - p.x), std::min(p.y, extentMetres - p.y));
  const double fade = std::clamp(edge / std::max(1.0, margin), 0.0, 1.0);
  return {dx * fade, dy * fade};
}
size_t TerrainMapSource::bucket(Point2 p) const
{
  const auto cell = [&](double v) { return uint32_t(std::clamp(std::floor(v / extentMetres * bucketSide_), 0.0, double(bucketSide_ - 1))); };
  return size_t(cell(p.y)) * bucketSide_ + cell(p.x);
}
void TerrainMapSource::addToBuckets(std::vector<std::vector<uint32_t>> &buckets, Point2 a, Point2 b, double radius, uint32_t id)
{
  const size_t lo = bucket({std::min(a.x, b.x) - radius, std::min(a.y, b.y) - radius});
  const size_t hi = bucket({std::max(a.x, b.x) + radius, std::max(a.y, b.y) + radius});
  for (size_t y = lo / bucketSide_; y <= hi / bucketSide_; ++y)
    for (size_t x = lo % bucketSide_; x <= hi % bucketSide_; ++x)
      buckets[y * bucketSide_ + x].push_back(id);
}
uint32_t TerrainMapSource::regionNode(Point2 p) const
{
  if (regionBuckets_.empty()) return world::kInvalidNode;
  const Point2 offset = regionWarp(p);
  p.x += offset.x; p.y += offset.y;
  // Nudge only world-rim queries into the polygon domain; this is not a raster
  // lookup. Shared polygon boundaries use deterministic node order.
  p.x = std::clamp(p.x, 1e-7, extentMetres - 1e-7);
  p.y = std::clamp(p.y, 1e-7, extentMetres - 1e-7);
  for (const auto id : regionBuckets_[bucket(p)])
    if (contains(graph_.node(id).area, p)) return id;
  return world::kInvalidNode;
}
double TerrainMapSource::coastDistance(Point2 p) const
{
  double closest = shoreWidth_;
  if (!coastBuckets_.empty())
    for (const auto id : coastBuckets_[bucket(p)])
      closest = std::min(closest, segmentDistance(p, coasts_[id].a, coasts_[id].b));
  return closest;
}
double TerrainMapSource::coastalHeight(Point2 p, const bool landKnown) const
{
  double ground = linear(terrainElevation_, p);
  if (!rockDisplacement_.empty() && (landKnown || !waterType(p)))
    ground += rockAt(p).height - linear(preRockElevation_,p) - linear(rockDisplacement_,p);
  const double t = std::clamp(coastDistance(p) / shoreWidth_, 0.0, 1.0);
  if (t >= 1.0) return ground;
  // Zero value and derivative at the waterline, and the original surface and
  // derivative at the inland end. This removes walls without raising lowlands.
  return seaLevelMetres + std::max(0.0, ground - seaLevelMetres) * t * t * (3.0 - 2.0 * t);
}
void TerrainMapSource::constructMountainRelief()
{
  // The exported refined field contains cell-aligned incision artefacts. Keep
  // it available as raw data, but construct the surface from clean base relief.
  terrainElevation_ = baseElevation_;
  // Gaussian reconstruction removes the coarse source cell shoulders without
  // terracing or disturbing the broad continental elevation distribution.
  const double sigma = 180.0 / cellMetres();
  const int radius = int(std::ceil(3 * sigma));
  std::vector<double> weights(size_t(2 * radius + 1));
  double sum = 0;
  for (int k = -radius; k <= radius; ++k) sum += weights[size_t(k+radius)] = std::exp(-.5*k*k/(sigma*sigma));
  for (auto &w : weights) w /= sum;
  std::vector<double> scratch(terrainElevation_.size());
  for (int axis = 0; axis < 2; ++axis)
  {
    for (uint32_t y = 0; y < resolution; ++y) for (uint32_t x = 0; x < resolution; ++x)
    {
      double value = 0;
      for (int k = -radius; k <= radius; ++k)
      {
        const int xx = std::clamp(int(x) + (axis == 0 ? k : 0), 0, int(resolution)-1);
        const int yy = std::clamp(int(y) + (axis == 1 ? k : 0), 0, int(resolution)-1);
        value += weights[size_t(k+radius)] * terrainElevation_[size_t(yy)*resolution+xx];
      }
      scratch[size_t(y)*resolution+x] = value;
    }
    terrainElevation_.swap(scratch);
  }
  std::vector<double> mountainMask(elevation_.size());
  uplift_.assign(elevation_.size(),0);
  // How high the ranges stand is decided from the area of land there is to
  // support them, and their shape by the orogeny chosen per component - see
  // TerrainOrogeny.hpp. There is deliberately no cap in metres here: the cap
  // there used to be is what gave the island one flat-topped massif, because
  // clipping every node of a range to the same number makes a plateau.
  mountains = buildMountainSkeleton(graph_, edges_, [this](Point2 p) { return linear(baseElevation_, p); },
      [this](Point2 p) { return linear(relief_, p); }, seaLevelMetres, seed);
  ridgeBuckets_.assign(bucketSide_ * bucketSide_, {});
  for (uint32_t i = 0; i < mountains.edges.size(); ++i)
  {
    const auto &edge = mountains.edges[i];
    const auto &a = mountains.nodes[edge.from], &b = mountains.nodes[edge.to];
    addToBuckets(ridgeBuckets_, a.position, b.position, std::max(a.widthMetres, b.widthMetres) * 1.4 + 400, i);
  }
  maximumMountainDetailMetres = 0;
  for (uint32_t y = 0; y < resolution; ++y)
    for (uint32_t x = 0; x < resolution; ++x)
    {
      const size_t at = size_t(y) * resolution + x;
      const Point2 p((x + .5) * cellMetres(), (y + .5) * cellMetres());
      if (waterType(p)) continue;
      double skeletonRise = 0, upliftStrength = 0;
      // Warp the ridge profile continuously, keeping the graph as its support.
      const Point2 q(p.x + 220*noise(p,2200,seed+21), p.y + 220*noise(p,2200,seed+29));
      for (const auto id : ridgeBuckets_[bucket(p)])
      {
        const auto &edge = mountains.edges[id];
        const auto &a = mountains.nodes[edge.from], &b = mountains.nodes[edge.to];
        skeletonRise = std::max(skeletonRise, ridgeProfileHeight(q, a, b));
        upliftStrength = std::max(upliftStrength, ridgeUpliftHeight(q, a, b));
      }
      // Noise is limited to a small fraction of the graph-shaped relief.
      // It cannot create a disconnected mountain outside the ridge envelope.
      const double perturbation = .22*noise(p,1100,seed+53)+.09*noise(p,360,seed+67);
      const double mask = smooth(coastDistance(p) / shoreWidth_);
      const double detail = mask * skeletonRise * (1+perturbation);
      mountainMask[at] = smooth(detail / 350.0);
      // Low-amplitude continuous plains relief, independent of region IDs.
      terrainElevation_[at] += mask * (3*noise(p,1800,seed+81)+noise(p,500,seed+83));
      terrainElevation_[at] += detail;
      // Compact ridge primitives now also drive persistent uplift, rather than
      // relying only on an elevation added once before surface erosion.
      uplift_[at] = mask * (.00015 + .022 * std::clamp(upliftStrength/3000.0,0.0,1.0));
      if (variedBiomes)
      {
        const double dry = smooth((.5-moistureAt(p))/.35);
        const double province = smooth(noise(p,extentMetres*.16,seed+501)+.4);
        const double folded = std::pow(.5+.5*std::sin(p.x/2200+p.y/9000+3*noise(p,8000,seed+507)),4);
        terrainElevation_[at] += mask*province*dry*(260*folded+80*noise(p,3200,seed+509));
        uplift_[at] += mask*province*dry*.002*folded;
      }
      maximumMountainDetailMetres = std::max(maximumMountainDetailMetres, std::abs(detail));
    }
  // One geographic ceiling over the finished relief.
  //
  // The ranges are raised to the height the landmass supports, but they are
  // raised *on top of* an imported base field that already carries relief of
  // its own, so the two sum to something the landmass does not support: a 1,600
  // square kilometre island came out with a 3,516 m summit, where the real ones
  // of that size stand around a thousand - Skye is 1,656 km^2 and 992 m. The
  // skeleton already knows the answer, because its tallest node was set from
  // the area; anything above that is the base field showing through, so the
  // whole above-water relief is scaled back to it. Scaling rather than clipping
  // is the point: clipping every node to one number is what made the plateau.
  {
    double peak = 0;
    for (const auto &node : mountains.nodes) peak = std::max(peak, node.riseMetres);
    if (peak > 0)
    {
      double tallest = 0;
      for (uint32_t y = 0; y < resolution; ++y)
        for (uint32_t x = 0; x < resolution; ++x)
        {
          const Point2 p((x + .5) * cellMetres(), (y + .5) * cellMetres());
          if (waterType(p)) continue;
          tallest = std::max(tallest, terrainElevation_[size_t(y)*resolution+x] - seaLevelMetres);
        }
      // A little headroom, because the base field is entitled to lift the
      // summit somewhat - a range does not begin at sea level.
      const double ceiling = peak * 1.15;
      if (tallest > ceiling)
      {
        const double scale = ceiling / tallest;
        for (uint32_t y = 0; y < resolution; ++y)
          for (uint32_t x = 0; x < resolution; ++x)
          {
            const size_t at = size_t(y)*resolution+x;
            const double above = terrainElevation_[at] - seaLevelMetres;
            if (above > 0) terrainElevation_[at] = seaLevelMetres + above * scale;
          }
        maximumMountainDetailMetres *= scale;
      }
    }
  }
  constructRockFormations();
  // Cut the scarps into the ground before the solver, so drainage, landslide
  // and diffusion all work on them. This is the difference between a cliff that
  // has been eroded and one that has an erosion-shaped profile drawn on it.
  if(streamPower.enabled) evolveLandscape();
  else if(hydraulics.enabled)
    erosion = erodeMountainHydraulics(resolution, cellMetres(), seed ^ 0x6879647261756cull,
        mountainMask, terrainElevation_, hydraulics);
  // Cut the scarps into ground the solver has already shaped, so they follow
  // its drainage and its contours, then weather them at raster resolution -
  // the solver's own grid is too coarse to hold a cliff at all.
  constructScarps();
  sharpenScarps();
}
double TerrainMapSource::moistureAt(Point2 p) const
{
  const double x=p.x/extentMetres+.10*noise(p,extentMetres*.25,seed+411);
  const double y=p.y/extentMetres+.12*noise(p,extentMetres*.28,seed+413);
  const double dryProvince=std::exp(-std::pow((x-.57)/.37,2)-std::pow((y-.32)/.18,2));
  const double secondary=std::exp(-std::pow((x-.21)/.13,2)-std::pow((y-.64)/.2,2));
  const double upwind=linear(baseElevation_,{p.x-extentMetres*.06,p.y});
  const double rainShadow=std::max(0.0,upwind-linear(baseElevation_,p))/1800;
  return std::clamp(.8-.78*dryProvince-.35*secondary+.32*noise(p,extentMetres*.17,seed+401)-.25*rainShadow,.06,.98);
}
void TerrainMapSource::evolveLandscape()
{
  const uint32_t side=std::min(resolution,std::max(4u,streamPower.maximumResolution));
  const double cell=extentMetres/side;
  const size_t count=size_t(side)*side;
  std::vector<double> h(count),u(count),rain(count),hardness(count);
  std::vector<uint8_t> outlet(count);
  for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x)
  {
    const size_t id=size_t(y)*side+x;
    const Point2 p((x+.5)*cell,(y+.5)*cell);
    outlet[id]=waterType(p)!=0 || x==0||y==0||x+1==side||y+1==side;
    h[id]=waterType(p)?waterLevel(p):coastalHeight(p,true);
    u[id]=linear(uplift_,p);
    const double wet=variedBiomes?linear(moisture_,p):.65;
    rain[id]=outlet[id]?0:std::max(.15,wet*1.5);
    hardness[id]=variedBiomes?.6+.8*smooth(noise(p,extentMetres*.13,seed+601)+.5):1;
    if(!rockHardness_.empty())hardness[id]*=linear(rockHardness_,p);
  }
  const auto before=h;
  streamPowerReport=erodeStreamPower(side,cell,u,rain,hardness,outlet,h,streamPower);
  const auto sample=[&](const std::vector<double>& f,Point2 p)
  {
    const double x=std::clamp(p.x/cell-.5,0.0,double(side-1)),y=std::clamp(p.y/cell-.5,0.0,double(side-1));
    const uint32_t x0=uint32_t(x),y0=uint32_t(y),x1=std::min(x0+1,side-1),y1=std::min(y0+1,side-1);
    const double tx=x-x0,ty=y-y0;
    return (f[size_t(y0)*side+x0]*(1-tx)+f[size_t(y0)*side+x1]*tx)*(1-ty)+
        (f[size_t(y1)*side+x0]*(1-tx)+f[size_t(y1)*side+x1]*tx)*ty;
  };
  for(uint32_t y=0;y<resolution;++y)for(uint32_t x=0;x<resolution;++x)
  {
    const Point2 p((x+.5)*cellMetres(),(y+.5)*cellMetres());
    if(waterType(p))continue;
    // Transfer the erosion displacement, retaining native-resolution detail.
    terrainElevation_[size_t(y)*resolution+x]+=sample(h,p)-sample(before,p);
  }
}
void TerrainMapSource::setGeometry(world::MapGraph graph, world::MapExportFeatures features)
{
  graph_ = std::move(graph); features_ = std::move(features);
  moisture_.resize(size_t(resolution)*resolution);
  for(uint32_t y=0;y<resolution;++y)for(uint32_t x=0;x<resolution;++x)
    moisture_[size_t(y)*resolution+x]=moistureAt({(x+.5)*cellMetres(),(y+.5)*cellMetres()});
  if(variedBiomes)for(uint32_t id=0;id<graph_.nodeCount();++id)
  {
    auto &n=graph_.node(id); if(n.refined()||!n.land||n.type==world::CellType::Lake)continue;
    const double moisture=moistureAt(n.site);
    const double temperature=30-32*n.site.y/extentMetres+12*noise(n.site,extentMetres*.3,seed+419)-.005*std::max(0.0,linear(baseElevation_,n.site));
    using world::Biome;
    n.biome=uint8_t(temperature<0?Biome::Tundra:temperature<7?Biome::Taiga:
        moisture<.19?Biome::SandDesert:moisture<.3?Biome::Desert:moisture<.42?Biome::Steppe:
        moisture<.56?Biome::Grassland:moisture<.74?Biome::TemperateForest:
        temperature>22?Biome::TropicalRainforest:Biome::TemperateRainforest);
  }
  edges_ = world::buildEdgeSet(graph_, std::max(1e-5, extentMetres * 1e-9));
  world::EdgeRelaxSettings boundary;
  boundary.segmentMetres = std::min(100.0, cellMetres());
  boundary.amplitudeMetres = std::min(180.0, extentMetres * .0014);
  boundary.wavelengthMetres = 1800;
  boundary.classificationScale = 1;
  boundary.seed = seed;
  world::relaxEdges(edges_, boundary);
  for (uint32_t id = 0; id < graph_.nodeCount(); ++id)
    if (!graph_.node(id).refined())
    {
      auto polygon = world::cellBoundary(edges_, id);
      if (polygon.size() >= 3) graph_.node(id).area = std::move(polygon);
    }
  regionBuckets_.assign(bucketSide_ * bucketSide_, {});
  coastBuckets_.assign(bucketSide_ * bucketSide_, {});
  channelBuckets_.assign(bucketSide_ * bucketSide_, {});
  coasts_.clear(); channels_.clear();
  shoreWidth_ = std::clamp(extentMetres * 0.012, cellMetres() * 3.0, std::max(cellMetres() * 3.0, 1800.0));
  // The warp has to be a fraction of the partition it is warping, not a fixed
  // number of metres. At 150 m against the 700 m cells of a 256 km world it is
  // a fifth of a cell and reads as coastal detail; against the 300 m cells of a
  // 48 km island it is half a cell, and it tore the coastline into a scatter of
  // fragments. Measured once on each and the difference was not subtle.
  {
    double totalArea = 0; uint32_t counted = 0;
    for (const auto &node : graph_.nodes())
    {
      if (node.refined() || node.area.size() < 3u) continue;
      double twice = 0;
      for (size_t i = 0, n = node.area.size(); i < n; ++i)
      {
        const Point2 &a = node.area[i], &b = node.area[(i + 1u) % n];
        twice += a.x * b.y - b.x * a.y;
      }
      totalArea += std::abs(twice) * .5; ++counted;
    }
    if (counted > 0 && regionWarpMetres > 0)
    {
      const double cellDiameter = std::sqrt(totalArea / counted);
      regionWarpMetres = std::min(regionWarpMetres, cellDiameter * regionWarpCellFraction);
      regionWarpScaleMetres = std::min(regionWarpScaleMetres, cellDiameter * 3.0);
    }
  }
  for (const auto &node : graph_.nodes())
  {
    if (node.refined() || node.area.empty()) continue;
    Point2 lo = node.area.front(), hi = lo;
    for (const auto p : node.area)
    {
      lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y);
      hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
    }
    // Dilated by the warp: a warped query can reach this far outside the cell's
    // own box, and the uniform-bucket shortcut below must see the same
    // candidates or it will answer "all water" for a bucket the warp made land.
    addToBuckets(regionBuckets_, lo, hi, regionWarpReach(), node.id);
  }
  bucketWater_.assign(bucketSide_ * bucketSide_, 255);
  bucketBiome_ = bucketLandform_ = bucketWater_;
  for (size_t i = 0; i < regionBuckets_.size(); ++i)
  {
    if (regionBuckets_[i].empty()) continue;
    const auto water = [&](uint32_t id)
    { const auto &n = graph_.node(id); return uint8_t(n.type == world::CellType::Lake ? 2 : !n.land ? 1 : 0); };
    const auto first = regionBuckets_[i].front();
    uint8_t w = water(first), b = graph_.node(first).biome, f = uint8_t(graph_.node(first).type);
    for (const auto id : regionBuckets_[i])
    { if (water(id) != w) w = 255; if (graph_.node(id).biome != b) b = 255; if (uint8_t(graph_.node(id).type) != f) f = 255; }
    bucketWater_[i] = w; bucketBiome_[i] = b; bucketLandform_[i] = f;
  }
  // The coastline is taken from the water mask, not from the map-graph edges.
  //
  // Those edges are the coarse partition's own boundaries, and the warp then
  // displaces the *query* rather than the edge - so the two drift apart.
  // Measured on a 48 km island: the chain sat a median 180 m from where land
  // actually met water, only 9% of it within 50 m, and 13% more than 400 m out.
  // Every consumer downstream - the shore distance, the coastal height blend,
  // the sand band, the mesh's own coastline constraint - was therefore working
  // from a curve that is not the coast.
  //
  // Marching the mask gives one curve that is the coast by construction, and
  // all of them now share it.
  coastLines_.clear();
  {
    RegionContourSettings options;
    options.resolution = std::min(2048u, std::max(256u, resolution));
    options.simplifyMetres = std::max(8.0, cellMetres() * .45);
    options.minimumPerimeterMetres = std::max(240.0, cellMetres() * 6.0);
    coastLines_ = extractRegionContours(extentMetres,
        [this](Point2 p) { return waterType(p) == 0u; }, options);
    for (const auto &ring : coastLines_)
      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Point2 a = ring[i], b = ring[(i + 1u) % ring.size()];
        const uint32_t id = uint32_t(coasts_.size());
        coasts_.push_back({a, b});
        addToBuckets(coastBuckets_, a, b, shoreWidth_, id);
      }
  }
  constructMountainRelief();
  const size_t authoredRiverCount = features_.rivers.segments.size();
  // Priority-flood establishes an acyclic receiver tree on the NEW surface.
  // Water bodies are outlets, so new tributaries terminate at lakes or sea.
  // Original river reaches remain additional authored constraints.
  if ((!mountains.edges.empty() || rockFormations.size()) && resolution >= 16)
  {
    // Routing resolution is what decides whether a river follows the ground or
    // cuts across it. At 512 over 256 km a cell is 500 m, so a reach could only
    // ever be a chain of 500 m steps and every valley narrower than that was
    // invisible to it. The receiver tree is a priority flood - one pass, one
    // queue - so the finer grid costs a few seconds, not minutes.
    const uint32_t side = std::min(2048u, resolution);
    const double cell = extentMetres / side;
    const size_t count = size_t(side)*side;
    std::vector<double> filled(count), area(count, 1.0), runoff(count);
    std::vector<uint32_t> receiver(count, UINT32_MAX), order;
    std::vector<uint8_t> seen(count), water(count);
    const auto point = [&](uint32_t id) { return Point2((id%side+.5)*cell,(id/side+.5)*cell); };
    using Item = std::pair<double,uint32_t>;
    std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
    for (uint32_t id = 0; id < count; ++id)
    {
      const auto p = point(id);
      water[id] = waterType(p);
      filled[id] = water[id] ? waterLevel(p) : coastalHeight(p);
      // Perennial channels are not a mountain privilege - a lowland catchment
      // that gathers enough area carries a river too - but they are a climate
      // one. Runoff is what rain leaves behind after the ground takes its
      // share, so an arid cell contributes none and a desert grows no drainage
      // of its own. A river fed from wet highlands still crosses it, because
      // the accumulation below carries that upstream water through.
      const double wet = variedBiomes ? linear(moisture_, p) : .65;
      // Climate decides what a cell yields; elevation does not. Requiring
      // ground to stand 120 m above the sea before it contributed any runoff
      // meant a river gained nothing crossing the lowlands - which is most of
      // its course and most of its catchment - so discharge never accumulated.
      // Measured, the largest river on a 256 km world carried 8 cumecs and every
      // reach came out between 25 and 34 m wide: a confluence of two streams
      // produced a stream, which is exactly what it should not do.
      runoff[id] = !water[id] ? std::max(0.0, wet - .28) : 0.0;
      if (water[id] || id%side==0 || id%side==side-1 || id/side==0 || id/side==side-1)
      { seen[id]=1; queue.emplace(filled[id],id); }
    }
    while (!queue.empty())
    {
      const auto [level,id] = queue.top(); queue.pop(); order.push_back(id);
      for (int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx)
      {
        const int x=int(id%side)+dx,y=int(id/side)+dy;
        if((dx==0&&dy==0)||x<0||y<0||x>=int(side)||y>=int(side))continue;
        const uint32_t next=uint32_t(y)*side+uint32_t(x);
        if(seen[next])continue;
        seen[next]=1; receiver[next]=id;
        filled[next]=std::max(filled[next],level+1e-5*std::hypot(dx,dy));
        queue.emplace(filled[next],next);
      }
    }
    for(auto it=order.rbegin();it!=order.rend();++it)
      if(receiver[*it]!=UINT32_MAX)
      { area[receiver[*it]]+=area[*it]; runoff[receiver[*it]]+=runoff[*it]; }
    // Trace reaches, not cells.
    //
    // The receiver tree is eight-connected, so following it cell by cell gives
    // a path that can only hold eight headings and turns at right angles every
    // 125 m. Emitting one segment per cell handed the river builder a chord of
    // a single cell, which is far too short for any meander to exist inside -
    // so every river on the map came out as a dead straight octilinear
    // staircase, and no amount of work on the meander could change that.
    //
    // A reach runs from a channel head or a confluence down to the next
    // confluence or outlet. Traced whole and then smoothed, it is a curve the
    // meander machinery can work on.
    // One width law, used by both the reach polyline and the surface sections.
    const auto widthScaleFor=[](const double flow)
    {
      const double volume=std::clamp((std::log10(std::max(flow,1.0))-1.0)/2.0,0.0,1.0);
      return 6.0*(1.0+.35*volume);
    };
    riverReaches.clear();
    std::vector<uint8_t> channel(count, 0u);
    for(uint32_t id=0;id<count;++id)
      if(!water[id] && receiver[id]!=UINT32_MAX && runoff[id]*cell*cell>=4000000)channel[id]=1u;
    std::vector<uint32_t> inflow(count, 0u);
    for(uint32_t id=0;id<count;++id)
      if(channel[id] && receiver[id]!=UINT32_MAX)++inflow[receiver[id]];
    std::vector<uint8_t> walked(count, 0u);
    for(uint32_t start=0;start<count;++start)
    {
      if(!channel[start] || walked[start])continue;
      // Start only at a head or just below a confluence; anything else is the
      // middle of a reach and will be picked up by the walk that owns it.
      if(inflow[start]==1u)continue;
      std::vector<Point2> path;
      double outflow=0;
      for(uint32_t at=start;;)
      {
        walked[at]=1u;
        path.push_back(point(at));
        const double catchmentKm2=area[at]*cell*cell/1e6;
        const double wetness=runoff[at]/std::max(1.0,area[at]);
        outflow=0.42*std::pow(catchmentKm2,0.69)*std::clamp(wetness/0.35,0.08,1.7);
        const uint32_t next=receiver[at];
        if(next==UINT32_MAX){break;}
        path.push_back(point(next));
        if(water[next]||!channel[next]){break;}
        if(inflow[next]>1u){break;}   // the confluence starts the next reach
        at=next;
        if(walked[at])break;
      }
      if(path.size()<2u)continue;
      // Chaikin takes the lattice out of the traced path. Three rounds is
      // enough that no trace of the eight headings survives.
      for(uint32_t round=0;round<3u&&path.size()>2u;++round)
      {
        std::vector<Point2> rounded{path.front()};
        for(size_t k=0;k+1<path.size();++k)
        {
          const Point2 &a=path[k],&b=path[k+1];
          rounded.push_back({a.x+(b.x-a.x)*.25,a.y+(b.y-a.y)*.25});
          rounded.push_back({a.x+(b.x-a.x)*.75,a.y+(b.y-a.y)*.75});
        }
        rounded.push_back(path.back());
        path.swap(rounded);
      }
      // Meander the reach.
      //
      // Smoothing alone cannot produce one. The receiver tree follows steepest
      // descent over a surface the solver left smooth at 250 m, so a reach is
      // genuinely straight for kilometres and Chaikin only rounds its corners.
      // A real river of that length does not run straight: it wanders inside
      // its own floodplain, and the wander is a property of the reach, not of
      // the ground under it. Zero at both ends so confluences stay welded.
      {
        double span=0;
        for(size_t k=1;k<path.size();++k)span+=distance(path[k-1],path[k]);
        if(span>200.0&&path.size()>4u)
        {
          const uint64_t meander=mix(seed^(uint64_t(start)*0x9E3779B97F4A7C15ull));
          // Meander geometry is set by the channel, not by the reach. Both of
          // these are standard: the belt a river wanders across is on the order
          // of fifteen channel widths, and the wavelength of one bend is about
          // eleven. Taking the amplitude as a fraction of the *reach* instead
          // capped it near 150 m, and a 150 m wander spread over ten kilometres
          // of harmonics is a straight line to look at - which is what the map
          // showed.
          const double width=std::max(10.0,6.0*std::sqrt(std::max(outflow,0.5)));
          // Stylised, and deliberately so. A real meander on a 30 m channel has
          // a wavelength around 330 m and an amplitude around 80 m, which on a
          // 256 km map is invisible - that was the straight-looking version.
          // Taking the physical numbers literally instead produced 41 bends of
          // 225 m amplitude over a 10 km reach, which is a sawtooth: the
          // amplitude has to stay well under the wavelength or it is a hairpin,
          // not a bend. These keep the ratio a river has and the size a map can
          // show.
          const double wavelength=std::max(900.0,width*40.0);
          const double swing=std::min(span*.10,wavelength*.22);
          const double waves=std::clamp(span/wavelength,1.0,24.0);
          std::vector<Point2> waved=path;
          double walkLength=0;
          for(size_t k=1;k+1<path.size();++k)
          {
            walkLength+=distance(path[k-1],path[k]);
            const double t=walkLength/span;
            double wave=0;
            for(int harmonic=0;harmonic<3;++harmonic)
            {
              // Frequencies about the physical wavelength, not about the
              // reach: a long river carries many bends, not one long one.
              // All three harmonics stay near the base wavelength, so the
              // curve varies without turning into a higher-frequency buzz.
              const double frequency=waves*(.82+.36*double(mix(meander+uint64_t(harmonic))%5u)/4.0)*
                  (harmonic==0?1.0:harmonic==1?1.35:.62);
              const double phase=double(mix(meander+71u+uint64_t(harmonic))%65536u)*(6.283185307179586/65536.0);
              wave+=std::sin(6.283185307179586*frequency*t+phase)/(harmonic+1);
            }
            const Point2 &a=path[k-1],&b=path[k+1];
            const double dx=b.x-a.x,dy=b.y-a.y;
            const double l=std::hypot(dx,dy);
            if(l<1e-6)continue;
            // Taper only near the ends, so a long reach meanders along its
            // whole length rather than only in the middle.
            const double ends=std::clamp(std::min(t,1.0-t)/.06,0.0,1.0);
            const double offset=ends*ends*(3.0-2.0*ends)*swing*wave*.5;
            waved[k]={path[k].x-dy/l*offset,path[k].y+dx/l*offset};
          }
          path.swap(waved);
        }
      }
      // Kept as one reach, and also emitted as segments for the existing
      // consumers. The reach is what a stroke, a renderer or a placement pass
      // should read.
      {
        RiverReach reach;
        reach.path=path;
        reach.dischargeCumecs=outflow;
        reach.reachesWater=receiver[start]!=UINT32_MAX&&water[receiver[start]]!=0;
        reach.width.reserve(path.size());
        for(size_t k=0;k<path.size();++k)
        {
          // Width grows along the reach as it gathers: the head is narrower
          // than the mouth even inside one reach.
          const double t=path.size()<2u?1.0:double(k)/double(path.size()-1u);
          const double flowHere=outflow*(.55+.45*t);
          reach.width.push_back(std::max(5.0,widthScaleFor(flowHere)*std::sqrt(std::max(flowHere,0.25))));
        }
        riverReaches.push_back(std::move(reach));
      }
      for(size_t k=1;k<path.size();++k)
        features_.rivers.segments.push_back({path[k-1],path[k],outflow,1,world::kInvalidNode,
            k+1==path.size()&&receiver[start]!=UINT32_MAX&&water[receiver[start]]!=0});
    }
  }
  struct Node { Point2 point; double bed; uint32_t inflow = 0; std::vector<uint32_t> outgoing; };
  std::vector<Node> nodes;
  std::map<std::pair<int64_t, int64_t>, uint32_t> ids;
  std::vector<std::pair<uint32_t, uint32_t>> links;
  const auto nodeAt = [&](Point2 p, double bed)
  {
    const auto key = std::make_pair(int64_t(std::llround(p.x * 1000)), int64_t(std::llround(p.y * 1000)));
    const auto [it, added] = ids.emplace(key, uint32_t(nodes.size()));
    if (added) nodes.push_back({p, bed});
    else nodes[it->second].bed = std::min(nodes[it->second].bed, bed);
    return it->second;
  };
  for (size_t riverId = 0; riverId < features_.rivers.segments.size(); ++riverId)
  {
    const auto &river = features_.rivers.segments[riverId];
    if (riverId < authoredRiverCount)
    {
      // A pre-uplift straight reach may now cross a ridge. Keep its original
      // map constraint, but let the newly routed network drain that catchment.
      bool uplifted = false;
      for (int i = 0; i <= 8; ++i)
      {
        const double t = i/8.0;
        const Point2 p(river.from.x+(river.to.x-river.from.x)*t,river.from.y+(river.to.y-river.from.y)*t);
        uplifted |= linear(terrainElevation_,p)-linear(baseElevation_,p)>100;
      }
      if (uplifted) continue;
    }
    const double length = std::hypot(river.to.x - river.from.x, river.to.y - river.from.y);
    if (length < 1e-6) continue;
    const double flow = std::max(0.0, river.dischargeCumecs);
    // Depth scale is set with the width below, from the reach type.
    // Valley incision, separate from the hydraulic channel depth.
    //
    // Hydraulic geometry gives a truthful 0.6 to 2 m of water, and the terrain
    // was cut to exactly the water level - so the river sat *on* the ground
    // rather than in anything. Measured on the finished mesh, the median
    // incision 150 m either side of a reach was 0.14 m, which at a 4 m
    // tolerance is nothing: that is why no river was visible.
    //
    // A river valley is not the channel. It is what the river has cut over
    // geological time, tens of metres for a trunk stream, and it is the valley
    // that a landscape reads by. The stream-power solver would produce it, but
    // it runs on a 250 m grid and the reaches are placed after it, so the
    // incision has to be applied to the long profile directly. Flow grows
    // downstream, so incision grows downstream too, which keeps the profile
    // monotone and the water running downhill.
    const double valley = std::min(riverValleyMaximumMetres,
        riverValleyMetres * std::pow(std::max(flow, 1.0) / 50.0, .35));
    // Reach type, after the Rosgen classification Genevaux et al. use: slope and
    // flow together say what kind of channel this is, and the type sets the
    // width-to-depth ratio and the sinuosity. One width law for every river on
    // the map is why they all read the same - a mountain torrent and a lowland
    // trunk had the same proportions and the same wander.
    //
    // Slope is taken over the reach; flow is the routed discharge.
    const double fall = (waterType(river.from) ? waterLevel(river.from) : coastalHeight(river.from, true)) -
        (waterType(river.to) ? waterLevel(river.to) : coastalHeight(river.to, true));
    const double grade = std::max(0.0, fall) / std::max(1.0, length);
    // Confined torrent: steep, narrow, deep for its width, barely sinuous.
    // Meandering lowland: gentle, wide, shallow, strongly sinuous.
    // Braided: very gentle and very large, widest and shallowest of all.
    const double steepness = std::clamp((grade - .004) / .030, 0.0, 1.0);
    const double volume = std::clamp((std::log10(std::max(flow, 1.0)) - 1.0) / 2.0, 0.0, 1.0);
    const double braided = std::clamp(volume - steepness, 0.0, 1.0);
    // Width-to-depth follows the type: 8*sqrt(Q) is the hydraulic baseline,
    // narrowed in a gorge and broadened on a floodplain.
    const double widthScale = 6.0 * (1.0 - .45 * steepness + .55 * braided);
    const double depthScale = .30 * (1.0 + .85 * steepness - .40 * braided);
    const double half = std::max(5.0, widthScale * std::sqrt(flow));
    // Sinuosity is the other half of the type: a confined reach cannot wander,
    // a floodplain reach does little else.
    const double sinuosity = .35 + 1.25 * (1.0 - steepness) * (.35 + .65 * volume);
    const double depth = std::max(0.6, depthScale * std::pow(flow, 0.4));
    // The router hands us a straight chord between two graph nodes. A real
    // reach wanders inside its valley, so displace the interior perpendicular
    // to the chord: a tapered sum of harmonics, seeded per river, which is zero
    // at both ends so junctions stay welded to their node.
    const double dirX = (river.to.x - river.from.x) / length, dirY = (river.to.y - river.from.y) / length;
    const double normalX = -dirY, normalY = dirX;
    const uint64_t meanderSeed = mix(seed ^ (uint64_t(riverId) * 0x9E3779B97F4A7C15ull));
    // Meander amplitude scales with the reach but is capped against the channel
    // so a small stream does not swing like a trunk river.
    // ...and with the reach type: a gorge is straight because its walls hold it
    // there, a floodplain river wanders because nothing does.
    const double swing = std::min(0.12 * length * sinuosity, (25.0 * half + 40.0) * sinuosity);
    // Enough samples that the curve reads as a curve; the old spacing put one
    // point every cell and a meander cannot exist between two of those.
    // Sampling follows the curve it has to describe: a reach that swings hard
    // needs points to describe the swing, a straight trunk does not. Spacing is
    // taken from the meander wavelength and the channel width together, so a
    // tight bend and a wide river both get enough geometry - which is what the
    // river surface mesh is short of, being built from the bank chains alone
    // with refinement switched off.
    const double wavelength = std::max(60.0, length / 3.0);
    const double bendSpacing = wavelength / 14.0;
    const double widthSpacing = std::max(20.0, half * .9);
    const double spacing = std::clamp(std::min(bendSpacing, widthSpacing),
        std::max(12.0, cellMetres() * .12), cellMetres() * 0.5);
    const uint32_t count = std::max(2u, uint32_t(std::ceil(length / spacing)));
    std::vector<double> lateral(count + 1u, 0.0);
    for (uint32_t step = 1; step < count; ++step)
    {
      const double t = double(step) / count;
      double wave = 0;
      for (int harmonic = 0; harmonic < 3; ++harmonic)
      {
        const double frequency = 1.0 + double(mix(meanderSeed + uint64_t(harmonic)) % 3u);
        const double phase = double(mix(meanderSeed + 97u + uint64_t(harmonic)) % 65536u) * (6.283185307179586 / 65536.0);
        wave += std::sin(6.283185307179586 * frequency * t + phase) / (harmonic + 1);
      }
      lateral[step] = std::sin(3.14159265358979 * t) * swing * wave * 0.5;
    }
    // Prefer the low path. A meander that would climb out of the valley is cut
    // back towards the chord, which is both what water does and what keeps the
    // downstream bed propagation from carving a trench across a hillside.
    for (uint32_t step = 1; step < count; ++step)
    {
      const double t = double(step) / count;
      const Point2 base(river.from.x + (river.to.x - river.from.x) * t, river.from.y + (river.to.y - river.from.y) * t);
      const double onChord = linear(terrainElevation_, base);
      for (int attempt = 0; attempt < 3 && lateral[step] != 0.0; ++attempt)
      {
        const Point2 shifted(base.x + normalX * lateral[step], base.y + normalY * lateral[step]);
        if (linear(terrainElevation_, shifted) <= onChord + 2.0) break;
        lateral[step] *= 0.45;
      }
    }
    // One smoothing sweep: the attenuation above is per sample and can leave a
    // kink where one point was cut back and its neighbours were not.
    for (uint32_t sweep = 0; sweep < 2u; ++sweep)
    {
      std::vector<double> relaxed = lateral;
      for (uint32_t step = 1; step < count; ++step)
        relaxed[step] = 0.25 * lateral[step - 1u] + 0.5 * lateral[step] + 0.25 * lateral[step + 1u];
      lateral.swap(relaxed);
    }
    uint32_t previous = 0;
    for (uint32_t step = 0; step <= count; ++step)
    {
      const double t = double(step) / count;
      const Point2 p(river.from.x + (river.to.x - river.from.x) * t + normalX * lateral[step],
          river.from.y + (river.to.y - river.from.y) * t + normalY * lateral[step]);
      const double surface = waterType(p) ? waterLevel(p) : coastalHeight(p, true);
      // Never cut below the sea it is running to. With no incision asked for,
      // this must be exactly the surface: clamping to sea level regardless
      // raises reaches that legitimately sit at or under it, which moves the
      // river network for no reason.
      const double sunk = !(valley > 0) || waterType(p) ? surface
          : std::max(seaLevelMetres + .5, surface - valley);
      const uint32_t current = nodeAt(p, sunk - depth);
      if (step)
      {
        nodes[previous].outgoing.push_back(current); ++nodes[current].inflow;
        links.emplace_back(previous, current);
        channels_.push_back({nodes[previous].point, p, 0, 0, half, depth, 0, half, depth, flow});
      }
      previous = current;
    }
  }
  // Propagate downstream across shared junctions. No reach can rise above its
  // tributaries; lower-envelope carving avoids adding separate cuts twice.
  std::deque<uint32_t> ready;
  for (uint32_t i = 0; i < nodes.size(); ++i) if (!nodes[i].inflow) ready.push_back(i);
  while (!ready.empty())
  {
    const auto id = ready.front(); ready.pop_front();
    for (const auto next : nodes[id].outgoing)
    {
      nodes[next].bed = std::min(nodes[next].bed, nodes[id].bed);
      if (--nodes[next].inflow == 0) ready.push_back(next);
    }
  }
  std::vector<RiverSurfaceReach> riverReaches;
  for(uint32_t i=0;i<channels_.size();++i)
  {const auto &s=channels_[i];
    const double a=waterType(s.a)?waterLevel(s.a):waterType(s.b)?waterLevel(s.b):std::max(seaLevelMetres,nodes[links[i].first].bed+s.depth);
    const double b=waterType(s.b)?waterLevel(s.b):std::max(seaLevelMetres,nodes[links[i].second].bed+s.depth);
    riverReaches.push_back({s.a,s.b,s.flow,a,b,s.depth,s.depth,waterType(s.a)!=0,waterType(s.b)!=0});}
  rivers.build(riverReaches,extentMetres,seed);
  for (uint32_t i = 0; i < channels_.size(); ++i)
  {
    channels_[i].bedA = nodes[links[i].first].bed;
    channels_[i].bedB = nodes[links[i].second].bed;
    auto &s = channels_[i];
    const auto &profile=rivers.sections()[i];
    s.halfWidth=profile.widthA;s.halfWidthB=profile.widthB;
    s.depth=profile.depthA;s.depthB=profile.depthB;
    s.bedA=profile.levelA-profile.depthA;s.bedB=profile.levelB-profile.depthB;
    const double bankRise = std::max(coastalHeight(s.a), coastalHeight(s.b)) - std::min(s.bedA, s.bedB);
    // Resistant formations retain steeper canyon shoulders. The former
    // universal 1:4 bank slope erased whole plateau benches after erosion.
    const double resistance=rockHardness_.empty()?1.0:std::max(linear(rockHardness_,s.a),linear(rockHardness_,s.b));
    s.outerWidth = std::max(s.halfWidth,s.halfWidthB) + std::min(2000.0, std::max(std::max(s.halfWidth,s.halfWidthB) * 3.0, bankRise * 4.0/std::sqrt(resistance)));
    addToBuckets(channelBuckets_, s.a, s.b, s.outerWidth, i);
  }
  allocateLand();
}

void TerrainMapSource::allocateLand()
{
  // Built unconditionally: the refinement tolerance reads it whether or not any
  // land is allocated, and a field that exists only when parcels do would make
  // the mesh silently change with an unrelated flag.
  { auto config=detail; config.seed=seed^0x64657461696cull; detailField.build(extentMetres,config); }
  if(!settlements.enabled && !parcels.enabled)return;
  const auto slopeAt=[this](Point2 p)
  {
    const double d=cellMetres();
    return std::hypot(linear(terrainElevation_,{p.x+d,p.y})-linear(terrainElevation_,{p.x-d,p.y}),
        linear(terrainElevation_,{p.x,p.y+d})-linear(terrainElevation_,{p.x,p.y-d}))/(2*d);
  };
  if(settlements.enabled)
  {
    SettlementFields fields;
    fields.elevationMetres=[this](Point2 p){return coastalHeight(p,true);};
    fields.slope=[slopeAt](Point2 p){return slopeAt(p);};
    fields.waterType=[this](Point2 p){return waterType(p);};
    fields.waterLevelMetres=[this](Point2 p){return waterLevel(p);};
    fields.riverDistanceMetres=[this](Point2 p){return double(linear(riverDistance_,p));};
    fields.riverDischarge=[this](Point2 p){return discharge_.empty()?0.0:linear(discharge_,p);};
    fields.coastDistanceMetres=[this](Point2 p){return coastDistance(p);};
    fields.treeCover=[this](Point2 p) -> double
    { const auto b=static_cast<world::Biome>(biome(p));
      const bool wooded=b==world::Biome::TemperateForest||b==world::Biome::TemperateRainforest||
          b==world::Biome::Taiga||b==world::Biome::MontaneForest||
          b==world::Biome::TropicalSeasonalForest;
      return wooded?.85:b==world::Biome::Savanna?.35:.2; };
    fields.soilQuality=[this,slopeAt](Point2 p) -> double
    { const auto form=static_cast<world::CellType>(landform(p));
      const double gentle=1.0-std::clamp(slopeAt(p)/.25,0.0,1.0);
      return gentle*(form==world::CellType::Plain||form==world::CellType::Valley?.9:
          form==world::CellType::Hill?.6:.25); };
    // No temperature raster is kept, but the cold biomes are exactly the gate
    // this criterion wants, so read them instead of inventing a field.
    fields.temperatureCelsius=[this](Point2 p) -> double
    { const auto b=static_cast<world::Biome>(biome(p));
      return b==world::Biome::Glacier?-18.0:b==world::Biome::Tundra?-4.0:
          b==world::Biome::ColdDesert?2.0:b==world::Biome::Taiga?1.0:12.0; };
    fields.moisture=[this](Point2 p){return variedBiomes&&!moisture_.empty()?linear(moisture_,p):.6;};
    auto config=settlements; config.seed=seed;
    settlementNetwork=planSettlements(extentMetres,fields,config);
  }
  if(!parcels.enabled)return;
  // Blocks are map-graph cells: already a planar partition at roughly block
  // scale, already carrying biome and landform. Only ground that would actually
  // be worked is offered, which is what keeps the constraint budget bounded -
  // parcelling the whole world at field scale is tens of millions of points.
  std::vector<std::vector<Point2>> blocks;
  for(uint32_t id=0;id<graph_.nodeCount();++id)
  {
    const auto &node=graph_.node(id);
    // Leaves only. The graph is hierarchical, so a refined node and its
    // children both carry an `area` polygon covering the same ground; feeding
    // both made every parcel overlap a coarser one and produced boundary edges
    // kilometres long, where a leaf cell is a few hundred metres across.
    if(node.refined())continue;
    if(!node.land||node.type==world::CellType::Lake||node.area.size()<3u)continue;
    if(node.type==world::CellType::Mountain||node.type==world::CellType::Rocks||
        node.type==world::CellType::Cliff||node.type==world::CellType::Canyon)continue;
    if(waterType(node.site))continue;
    if(slopeAt(node.site)>parcels.maximumSlope)continue;
    blocks.push_back(node.area);
  }
  // Each ward is its own block, so lots are cut inside the ward the plan drew
  // rather than inside whatever graph cell happens to contain it. The ward is
  // the unit a town is made of; subdividing it is what puts buildings between
  // the streets instead of a single undivided blob inside the wall.
  // Which blocks are wards, so the street peel knows where to apply. A radius
  // test cannot stand in for this: a map-graph block kilometres across can
  // contain a town centre without being a ward.
  std::vector<uint8_t> wardBlocks(blocks.size(), 0u);
  for(const auto &plan:settlementNetwork.plans)
    for(const auto &district:plan.districts)
      if(district.boundary.size()>=3u)
      {blocks.push_back(district.boundary);wardBlocks.push_back(uint8_t(uint32_t(district.kind)+1u));}
  // Region contours. A sand band and a biome are regions like any other, but
  // they were the only two defined by a sample test rather than by a polygon -
  // so their edges were decided per triangle and came out as tongues. Marching
  // their own scalar gives them a boundary, and once it is a cutter the parcel
  // layer subdivides inside it and every cell is wholly one region.
  std::vector<std::vector<Point2>> regionEdges;
  if(regionContours.shoreBand)
  {
    RegionContourSettings options;
    options.resolution=regionContours.shoreResolution;
    options.simplifyMetres=regionContours.shoreSimplifyMetres;
    // The shore band is a distance iso-contour, so it is marched on the
    // distance itself and the crossing interpolated: the edge then follows the
    // true 95 m line instead of stepping along a half-cell lattice. Ocean is
    // pushed far negative so the contour hugs the waterline from the land side.
    const double band=regionContours.shoreBandMetres;
    auto rings=extractIsoContours(extentMetres,
        [this,band](Point2 p){return waterType(p)?-band:band-coastDistance(p);},0.0,options);
    shoreBandRings_=rings;
    regionEdges.insert(regionEdges.end(),std::make_move_iterator(rings.begin()),
        std::make_move_iterator(rings.end()));
  }
  if(regionContours.biomes)
  {
    RegionContourSettings options;
    options.resolution=regionContours.biomeResolution;
    options.simplifyMetres=regionContours.biomeSimplifyMetres;
    options.minimumPerimeterMetres=regionContours.biomeMinimumPerimeterMetres;
    auto rings=extractLabelContours(extentMetres,[this](Point2 p){return biome(p);},options);
    regionEdges.insert(regionEdges.end(),std::make_move_iterator(rings.begin()),
        std::make_move_iterator(rings.end()));
  }

  // Infrastructure cuts rather than being cut: river banks, the shoreline and
  // the roads between settlements.
  std::vector<std::vector<Point2>> infrastructure;
  for(auto &ring:regionEdges)
  { auto closed=ring; closed.push_back(closed.front()); infrastructure.push_back(std::move(closed)); }
  for(const auto &chain:rivers.chains())infrastructure.push_back(chain.points);
  // A road is cut in as its two edges, not its centre-line. The strip between
  // them then falls out of the subdivision as cells of its own, which is the
  // only way a seven metre carriageway can exist in a partition whose cells are
  // sixty metres across - classifying by proximity alone would have painted a
  // whole field as road.
  for(const auto &road:settlementNetwork.roads)
  {
    if(road.path.size()<2u){infrastructure.push_back(road.path);continue;}
    const double half=std::max(1.5,road.widthMetres*.5);
    for(int side=0;side<2;++side)
    {
      std::vector<Point2> edge;
      edge.reserve(road.path.size());
      for(size_t k=0;k<road.path.size();++k)
      {
        // Average the normals of the two segments at a vertex so the offset
        // turns the corner instead of breaking at it.
        const Point2 &here=road.path[k];
        double nx=0,ny=0;
        if(k)
        { const double dx=here.x-road.path[k-1].x,dy=here.y-road.path[k-1].y;
          const double l=std::hypot(dx,dy); if(l>1e-9){nx+=-dy/l;ny+=dx/l;} }
        if(k+1<road.path.size())
        { const double dx=road.path[k+1].x-here.x,dy=road.path[k+1].y-here.y;
          const double l=std::hypot(dx,dy); if(l>1e-9){nx+=-dy/l;ny+=dx/l;} }
        const double l=std::hypot(nx,ny);
        if(l<1e-9)continue;
        const double sign=side?-1.0:1.0;
        edge.push_back({here.x+nx/l*half*sign,here.y+ny/l*half*sign});
      }
      if(edge.size()>=2u)infrastructure.push_back(std::move(edge));
    }
  }
  // Streets and walls cut the built-up ground the same way roads cut the
  // country. This is what turns a settlement block into irregular blocks
  // between streets rather than pie wedges around a centre - the rosette is the
  // shape Watabou moved away from, and it was wrong here for the same reason.
  for(const auto &plan:settlementNetwork.plans)
  {
    for(const auto &street:plan.streets)infrastructure.push_back(street);
    if(!plan.wall.empty())
    { auto ring=plan.wall; ring.push_back(ring.front()); infrastructure.push_back(std::move(ring)); }
  }
  ParcelFields fields;
  fields.slope=[slopeAt](Point2 p){return slopeAt(p);};
  fields.waterType=[this](Point2 p){return waterType(p);};
  fields.elevationMetres=[this](Point2 p){return coastalHeight(p,true);};
  // Continuous, not a biome flag. Read as a threshold, a binary test made a
  // forest biome wall-to-wall wood - 40% of the parcelled area in single stands
  // kilometres across, with no farmland in them at all. Settled country has
  // woods as coherent patches inside farmland, so the biome sets the odds and a
  // noise field at wood scale decides which blocks actually carry one. The same
  // field lets a copse appear in open country, which is what a hedgerow
  // landscape looks like from the air.
  fields.treeCover=[this](Point2 p) -> double
  { const auto b=static_cast<world::Biome>(biome(p));
    const bool wooded=b==world::Biome::TemperateForest||b==world::Biome::TemperateRainforest||
        b==world::Biome::Taiga||b==world::Biome::MontaneForest||
        b==world::Biome::TropicalSeasonalForest;
    const double patch=noise(p,1700.0,seed^0x776f6f64ull)+.5*noise(p,640.0,seed^0x636f707365ull);
    // `noise` is signed, roughly -1..1, so these are a centre and a swing, not
    // a floor and a range. Read as 0..1 the first time, this left 2.6% of the
    // ground wooded where the references carry fifteen to twenty-five.
    return std::clamp((wooded?.52:.14)+.45*patch,0.0,1.0); };
  fields.soilQuality=[this,slopeAt](Point2 p) -> double
  { const auto form=static_cast<world::CellType>(landform(p));
    return (1.0-std::clamp(slopeAt(p)/.25,0.0,1.0))*
        (form==world::CellType::Plain||form==world::CellType::Valley?.9:.45); };
  fields.moisture=[this](Point2 p){return variedBiomes&&!moisture_.empty()?linear(moisture_,p):.6;};
  // The high-level biome decides whether this ground is farmed at all. The
  // parcel layer decorates the map; it must not contradict it. Without this the
  // allocation covered deserts, salt flats and icefields in hedged fields
  // wherever the slope happened to be gentle.
  fields.cultivable=[this](Point2 p) -> double
  { switch(static_cast<world::Biome>(biome(p)))
    {
    case world::Biome::Glacier: case world::Biome::SaltFlat:
    case world::Biome::Ocean:   case world::Biome::Lake:            return 0.0;
    case world::Biome::Desert:                                      return .02;
    case world::Biome::ColdDesert:                                  return .05;
    case world::Biome::Tundra:                                      return .12;
    case world::Biome::Taiga:                                       return .35;
    case world::Biome::Steppe:                                      return .55;
    case world::Biome::Shrubland: case world::Biome::Savanna:       return .70;
    case world::Biome::MontaneForest:                               return .55;
    case world::Biome::TropicalRainforest:                          return .45;
    default:                                                        return 1.0;
    } };
  fields.exposedRock=[this](Point2 p){return rockAt(p).role!=SurfaceRole::Ground;};
  fields.coastDistance=[this](Point2 p){return coastDistance(p);};
  // Membership of the sand band is decided by the same rings that cut it, so a
  // cell in the strip is sand whatever its centre's distance happens to be.
  // Indexed, because it is asked once per finished cell.
  {
    const uint32_t side=256u; const double cell=extentMetres/side;
    auto buckets=std::make_shared<std::vector<std::vector<uint32_t>>>(size_t(side)*side);
    auto rings=std::make_shared<std::vector<std::vector<Point2>>>(shoreBandRings_);
    for(uint32_t i=0;i<rings->size();++i)
    {
      const auto &ring=(*rings)[i];
      if(ring.size()<3u)continue;
      Point2 lo=ring.front(),hi=lo;
      for(const Point2 &q:ring)
      { lo.x=std::min(lo.x,q.x); lo.y=std::min(lo.y,q.y); hi.x=std::max(hi.x,q.x); hi.y=std::max(hi.y,q.y); }
      const auto x0=size_t(std::clamp(lo.x/cell,0.0,double(side-1))),x1=size_t(std::clamp(hi.x/cell,0.0,double(side-1)));
      const auto y0=size_t(std::clamp(lo.y/cell,0.0,double(side-1))),y1=size_t(std::clamp(hi.y/cell,0.0,double(side-1)));
      for(size_t y=y0;y<=y1;++y)for(size_t x=x0;x<=x1;++x)(*buckets)[y*side+x].push_back(i);
    }
    fields.inShoreBand=[buckets,rings,side,cell](Point2 p)
    {
      const auto gx=size_t(std::clamp(p.x/cell,0.0,double(side-1)));
      const auto gy=size_t(std::clamp(p.y/cell,0.0,double(side-1)));
      bool in=false;
      for(const uint32_t i:(*buckets)[gy*size_t(side)+gx])
      {
        const auto &ring=(*rings)[i];
        for(size_t a=0,n=ring.size(),b=n-1u;a<n;b=a++)
          if((ring[a].y>p.y)!=(ring[b].y>p.y) &&
              p.x<(ring[b].x-ring[a].x)*(p.y-ring[a].y)/(ring[b].y-ring[a].y)+ring[a].x)
            in=!in;
      }
      return in;
    };
  }
  fields.riverChannel=[this](Point2 p){return rivers.sample(p).inside;};
  fields.riverDistance=[this](Point2 p){return double(linear(riverDistance_,p));};
  auto config=parcels; config.seed=seed^0x70617263656cull;
  parcelLayer=allocateParcels(extentMetres,blocks,infrastructure,fields,settlementNetwork,config,wardBlocks);
}
} // namespace procedural::geometry
