#include "procedural/Geometry.hpp"
#include <array>
#include "procedural/PoissonDiskSampler.hpp"
#include "procedural/geometry/TerrainMapSource.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace procedural::geometry
{
namespace
{
uint64_t hash(uint64_t n){n^=n>>30;n*=0xbf58476d1ce4e5b9ull;n^=n>>27;n*=0x94d049bb133111ebull;return n^(n>>31);}
double random(uint64_t s){return double(hash(s)>>11)/double(1ull<<53);}
}
LandformSample TerrainMapSource::rockAt(Point2 p) const
{
  if(preRockElevation_.empty())return {};
  return rockFormations.sample(p,linear(preRockElevation_,p));
}
void TerrainMapSource::constructRockFormations()
{
  preRockElevation_.clear();rockDisplacement_.clear();rockHardness_.clear();rockFormations={};
  if(!landforms.enabled)return;
  std::vector<RockFormation> forms=landforms.authored;
  if(forms.empty() && resolution>=32)
  {
    // Candidate sites come from a Poisson disc over the map rather than from
    // the map graph's cells. The graph's cell density is a property of the
    // world generator, not of where rock ought to outcrop, and walking it tied
    // formation spacing to something unrelated. A disc gives even coverage at a
    // spacing we choose, and every geographic test below still has to pass.
    PoissonDiskSampler sampler;
    PoissonDiskSampler::Settings sampling{};
    sampling.radius=std::max(1500.0,extentMetres/48.0);
    sampling.width=extentMetres; sampling.height=extentMetres;
    sampling.seed=hash(seed^0x706c6163656d74ull);
    sampling.method=PoissonDiskSampler::Method::FillVoids;
    std::vector<Point2> sites=sampler.generate(sampling);
    // Larger structures first, so a scatter of outcrops cannot consume the
    // budget before a plateau has had a chance to fit.
    std::sort(sites.begin(),sites.end(),[&](const Point2 &a,const Point2 &b)
    {
      const uint64_t ha=hash(seed^uint64_t(std::llround(a.x*8))^(uint64_t(std::llround(a.y*8))<<21));
      const uint64_t hb=hash(seed^uint64_t(std::llround(b.x*8))^(uint64_t(std::llround(b.y*8))<<21));
      const bool smallA=random(ha+1)<.25, smallB=random(hb+1)<.25;
      const bool mountainA=linear(terrainElevation_,a)>1100, mountainB=linear(terrainElevation_,b)>1100;
      if(mountainA!=mountainB)return mountainA>mountainB;
      return smallA!=smallB?smallA<smallB:ha<hb;
    });
    uint32_t largeCount=0, mountainCount=0;
    struct Occupied{Point2 p;double radius;};std::vector<Occupied> occupied;
    for(const auto &c:sites)
    {
      if(forms.size()>=landforms.maximumCount)break;
      const auto nodeId=regionNode(c);
      if(nodeId==world::kInvalidNode)continue;
      const auto &node=graph_.node(nodeId);
      if(!node.land||node.type==world::CellType::Lake||waterType(c))continue;
      const double base=linear(terrainElevation_,c);
      if(base<seaLevelMetres+70)continue;
      const bool mountain=base>1100 || node.type==world::CellType::Mountain;
      const uint64_t s=hash(seed^uint64_t(std::llround(c.x*8))^(uint64_t(std::llround(c.y*8))<<21)^0x636c696666ull);
      // Formations grow with the map so a continent does not read as a plain
      // with pebbles, but the old 2.0 ceiling put 5 km footprints and 650 m
      // faces on a 256 km map, which is a mesa the size of a city.
      const double scale=std::clamp(extentMetres/128000.0,.2,1.15);
      const bool dry=moistureAt(c)<.45;
      const bool outcrop=random(s+1)<.25;
      if(mountain && mountainCount>=std::max(1u,landforms.maximumCount/3))continue;
      if(!outcrop && largeCount>=std::max(1u,landforms.maximumCount*3/4))continue;
      if(!dry && !mountain && random(s+10)<.65)continue;
      if(linear(riverDistance_,c)<cellMetres()*2)continue;
      const double radius=mountain?(520+380*random(s+2))*scale:(outcrop?420+560*random(s+2):760+900*random(s+2))*scale;
      // A perfectly round footprint is what makes an outcrop read as a blob
      // dropped on the terrain. Everything gets some elongation, and its axis
      // is already randomised below.
      const double elongation=outcrop?1.15+.55*random(s+3):mountain?1.25+.7*random(s+3):1.35+1.05*random(s+3);
      const double rise=outcrop?110+190*random(s+4):150+290*random(s+4);
      RockFormation f;f.seed=s;
      // Width from the same two-segment model the profile uses: the free face
      // at its own angle over the debris slope at the angle of repose. At
      // rise/2.1 every escarpment stood at 64 degrees, which is steeper than
      // scree can rest at - that is why they read as extruded - and it is also
      // a width the two-segment profile cannot fit into.
      {
        const double repose=std::tan(std::clamp(f.reposeAngleDegrees,5.0,60.0)*3.14159265359/180.0);
        const double share=std::clamp(f.freeFaceFraction,0.0,.9);
        const double steep=repose*2.4;
        f.faceWidthMetres=std::max(80.0,rise*(share/steep+(1-share)/repose)*(dry?.92:1.0));
      }
      f.talusWidthMetres=rise*(dry?1.5:2.2);
      f.rampAzimuth=random(s+9)*6.28318530718;
      f.rampWidthMetres=outcrop?0:rise*(dry?3.5:7.0);
      f.stratumMetres=dry?45+60*random(s+5):90+90*random(s+5);
      // Style follows climate; geological rock suite follows the source cell.
      f.lithology=outcrop?PlanetLithology::Granite:lithologyForCell(node);
      const double support=radius*elongation*1.5+f.faceWidthMetres*1.22+f.rampWidthMetres+f.talusWidthMetres;
      if(c.x<support||c.y<support||c.x+support>extentMetres||c.y+support>extentMetres)continue;
      bool valid=true;for(const auto &o:occupied)if(distance(c,o.p)<support+o.radius){valid=false;break;}
      if(!valid)continue;
      double low=base,high=base;
      const double angle=random(s+6)*6.28318530718;
      // The rim follows the ground the formation stands on instead of being a
      // rosette laid over it. March outward along each direction and stop where
      // the terrain has fallen away from this bench, or where it climbs above
      // it - a mesa ends at the edge of the bench, and it does not swallow a
      // neighbouring summit. That is what makes the footprint read as geology
      // rather than as a stamp.
      constexpr uint32_t kRimCount=24;
      const double dropLimit=std::max(40.0,rise*.45);
      const double step=std::max(cellMetres(),radius/24.0);
      std::array<double,kRimCount> reach{};
      for(uint32_t i=0;i<kRimCount;++i)
      {
        const double a=i*6.28318530718/kRimCount;
        const double ux=std::cos(a)*elongation,uy=std::sin(a);
        const double dirX=std::cos(angle)*ux-std::sin(angle)*uy;
        const double dirY=std::sin(angle)*ux+std::cos(angle)*uy;
        const double span=std::hypot(dirX,dirY);
        double travelled=step;
        for(;travelled<radius*elongation;travelled+=step)
        {
          const Point2 probe(c.x+dirX/span*travelled,c.y+dirY/span*travelled);
          const double at=linear(terrainElevation_,probe);
          if(at<base-dropLimit||at>base+dropLimit)break;
        }
        reach[i]=std::clamp(travelled,radius*.3,radius*elongation);
      }
      // One smoothing sweep around the ring: a single probe that stepped off a
      // gully would otherwise leave a spike in the outline.
      std::array<double,kRimCount> smoothed=reach;
      for(uint32_t i=0;i<kRimCount;++i)
        smoothed[i]=.25*reach[(i+kRimCount-1)%kRimCount]+.5*reach[i]+.25*reach[(i+1)%kRimCount];
      for(uint32_t i=0;i<kRimCount;++i)
      {
        const double a=i*6.28318530718/kRimCount;
        // Keep a little authored irregularity so a rim that happens to sit on
        // even ground is not a polygon.
        const double r=smoothed[i]*(1+.09*std::sin(3*a+random(s+13)*6.28)+.05*(random(s+20+i)-.5));
        const double x=std::cos(a)*r,y=std::sin(a)*r;
        const Point2 p(c.x+std::cos(angle)*x-std::sin(angle)*y,c.y+std::sin(angle)*x+std::cos(angle)*y);
        f.controls.push_back(p);
      }
      // Reject supports crossing water; the center avoids established rivers. Interior samples
      // catch small lakes, not just water intersecting the footprint boundary.
      for(int y=-4;y<=4;++y)for(int x=-4;x<=4;++x)
      {
        const Point2 p(c.x+x*support/4,c.y+y*support/4);
        if(waterType(p)||coastDistance(p)<shoreWidth_)valid=false;
        const double h=linear(terrainElevation_,p);low=std::min(low,h);high=std::max(high,h);
      }
      if(!valid||high-low> (mountain?2200.0:rise*1.6))continue;
      f.upliftMetresPerYear=linear(uplift_,c);
      f.cutCap=mountain;
      f.topMetres=mountain?base+std::min(rise,200.0):high+rise;
      forms.push_back(std::move(f));occupied.push_back({c,support});
      largeCount+=!outcrop;mountainCount+=mountain;
    }
  }
  if(forms.empty())return;
  if(landforms.authored.empty())forms=nestRockFormations(std::move(forms),landforms.cliffLevels,
      std::max(30.0,std::min(120.0,cellMetres())));
  rockFormations.build(std::move(forms),extentMetres,std::max(30.0,std::min(120.0,cellMetres())));
  preRockElevation_=terrainElevation_;
  rockDisplacement_.resize(terrainElevation_.size());rockHardness_.resize(terrainElevation_.size(),1);
  for(uint32_t y=0;y<resolution;++y)for(uint32_t x=0;x<resolution;++x)
  {
    const size_t id=size_t(y)*resolution+x;const Point2 p((x+.5)*cellMetres(),(y+.5)*cellMetres());
    if(waterType(p))continue;
    const auto value=rockFormations.sample(p,preRockElevation_[id]);
    rockDisplacement_[id]=value.height-preRockElevation_[id];
    rockHardness_[id]=value.hardness;terrainElevation_[id]=value.height;
    // A resistant cap is a coherent uplift block. Reusing the original
    // point-varying ridge uplift would bend a flat bench back into a dome.
    uplift_[id]+=(value.upliftMetresPerYear-uplift_[id])*value.influence;
  }
}

// --- contour scarps ---------------------------------------------------------
// A different construction from the spline rock formations above, and a more
// truthful one. Those *raise* a plateau with an analytic face profile pasted on
// top of the finished surface, which is why erosion can never touch them: the
// profile is re-applied after the solver has run.
//
// A scarp instead *cuts* the existing ground. A spline is traced along a
// contour - perpendicular to the gradient, so it follows the shape the terrain
// already has - and everything on the downhill side is lowered: sharply at the
// line, then flattening out until it rejoins the untouched surface a long way
// off. The ends taper to nothing so the cliff dies into the hillside rather
// than stopping dead.
//
// Because this writes into the elevation field *before* the stream-power
// solver, erosion, landslide and drainage all act on it - which is the whole
// point, and what no amount of work on the analytic profile could buy.
void TerrainMapSource::constructScarps()
{
  scarpLines_.clear();
  if (!scarps.enabled || scarps.count == 0u || resolution < 32u) return;
  const double cell = cellMetres();
  const auto sample = [&](const int64_t x, const int64_t y) -> double
  {
    const auto cx = size_t(std::clamp<int64_t>(x, 0, int64_t(resolution) - 1));
    const auto cy = size_t(std::clamp<int64_t>(y, 0, int64_t(resolution) - 1));
    return terrainElevation_[cy * resolution + cx];
  };
  const auto heightAt = [&](const Point2 p)
  { return sample(int64_t(p.x / cell), int64_t(p.y / cell)); };
  // Uphill direction, from the raster.
  const auto gradient = [&](const Point2 p)
  {
    const int64_t x = int64_t(p.x / cell), y = int64_t(p.y / cell);
    return Point2((sample(x + 1, y) - sample(x - 1, y)) / (2 * cell),
        (sample(x, y + 1) - sample(x, y - 1)) / (2 * cell));
  };
  uint64_t state = seed ^ 0x7363617270ull;
  const auto random = [&]()
  { state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return double(state >> 11) / double(1ull << 53); };

  // An escarpment stands because something at its brink resists. Erosion is
  // what this generator hands its cliffs to, so without a resistant band the
  // solver simply removes them - measured, the slope distribution came out
  // gentler with scarps than without.
  if (scarps.resistance > 1.0 && rockHardness_.size() != terrainElevation_.size())
    rockHardness_.assign(terrainElevation_.size(), 1.0);

  uint32_t made = 0u;
  for (uint32_t attempt = 0u; attempt < scarps.count * 40u && made < scarps.count; ++attempt)
  {
    Point2 at(random() * extentMetres, random() * extentMetres);
    if (waterType(at)) continue;
    const Point2 slope = gradient(at);
    const double steepness = std::hypot(slope.x, slope.y);
    // Somewhere with a definite downhill, but not already a precipice.
    if (steepness < scarps.minimumSlope || steepness > scarps.maximumSlope) continue;
    if (heightAt(at) < seaLevelMetres + 40.0) continue;

    // Trace the contour both ways: step perpendicular to the gradient, with a
    // little wander, re-reading the gradient each step so the line keeps
    // following the ground rather than drifting off it.
    const double length = scarps.lengthMetres * (.6 + .9 * random());
    const double step = std::max(cell, scarps.lengthMetres / 90.0);
    std::vector<Point2> line{at};
    for (int direction = 0; direction < 2; ++direction)
    {
      Point2 walk = at;
      const double wanderSeed = random() * 1000.0;
      for (uint32_t k = 1; k * step < length * .5; ++k)
      {
        const Point2 g = gradient(walk);
        const double magnitude = std::hypot(g.x, g.y);
        if (magnitude < 1e-9) break;
        // Along the contour is perpendicular to the gradient.
        double ax = -g.y / magnitude, ay = g.x / magnitude;
        if (direction) { ax = -ax; ay = -ay; }
        const double bend = scarps.wander *
            std::sin(k * step / scarps.wanderScaleMetres + wanderSeed);
        const double cosB = std::cos(bend), sinB = std::sin(bend);
        const Point2 next(walk.x + (ax * cosB - ay * sinB) * step,
            walk.y + (ay * cosB + ax * sinB) * step);
        if (next.x < cell || next.y < cell || next.x > extentMetres - cell ||
            next.y > extentMetres - cell) break;
        if (waterType(next)) break;
        walk = next;
        if (direction) line.insert(line.begin(), walk); else line.push_back(walk);
      }
    }
    if (line.size() < 8u) continue;

    // Cut the downhill side. The drop is full just past the line and eases to
    // nothing at the runout, and it tapers along the line so the ends rejoin
    // the hillside.
    const double drop = scarps.dropMetres * (.55 + .9 * random());
    const double runout = scarps.runoutMetres * (.7 + .6 * random());
    // The face is a raster cell or two, which is as sharp as this field can
    // express. Two earlier attempts widened it instead - once to fit the
    // erosion grid's 250 m cells, once to let the solver dissect it - and both
    // measured as a *reduction* in slope, because a 110 m drop spread over
    // 625 m is a gradient of 0.18: gentler than the hillside it replaced. A
    // cliff at this resolution is a one-cell step or it is nothing.
    const double face = std::max(cell * 1.1, scarps.faceMetres);
    Point2 low = line.front(), high = line.front();
    for (const Point2 &p : line)
    { low.x = std::min(low.x, p.x); low.y = std::min(low.y, p.y);
      high.x = std::max(high.x, p.x); high.y = std::max(high.y, p.y); }
    const auto x0 = size_t(std::max(0.0, (low.x - runout) / cell));
    const auto x1 = size_t(std::min(double(resolution - 1), (high.x + runout) / cell));
    const auto y0 = size_t(std::max(0.0, (low.y - runout) / cell));
    const auto y1 = size_t(std::min(double(resolution - 1), (high.y + runout) / cell));
    for (size_t y = y0; y <= y1; ++y)
      for (size_t x = x0; x <= x1; ++x)
      {
        const Point2 p((x + .5) * cell, (y + .5) * cell);
        // Nearest point on the line, its side, and how far along it we are.
        double best = std::numeric_limits<double>::max();
        double side = 0, along = 0;
        for (size_t k = 1; k < line.size(); ++k)
        {
          const Point2 &a = line[k - 1], &b = line[k];
          const double dx = b.x - a.x, dy = b.y - a.y;
          const double span = dx * dx + dy * dy;
          if (span < 1e-9) continue;
          const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / span, 0.0, 1.0);
          const double qx = p.x - a.x - t * dx, qy = p.y - a.y - t * dy;
          const double distance = std::hypot(qx, qy);
          if (distance >= best) continue;
          best = distance;
          side = qx * dy - qy * dx;          // sign tells which side
          along = (double(k - 1) + t) / double(line.size() - 1);
        }
        if (best > runout) continue;
        // Which side is downhill is decided by height, not by the winding of
        // the polyline: the cut belongs on whichever side already sits lower
        // than the line itself.
        (void)side;
        const double reference = heightAt(line[std::min(line.size() - 1,
            size_t(along * double(line.size() - 1)))]);
        if (terrainElevation_[y * resolution + x] > reference) continue;
        // Taper to nothing at the two ends of the line.
        const double ends = std::min(1.0, std::min(along, 1.0 - along) / scarps.taper);
        const double smoothEnds = ends * ends * (3.0 - 2.0 * ends);
        // Full drop within the face, easing to zero by the runout: the flat
        // bench below the cliff, meeting the untouched ground far away.
        const double t = std::clamp((best - face) / std::max(1.0, runout - face), 0.0, 1.0);
        const double ease = 1.0 - t * t * (3.0 - 2.0 * t);
        const double cut = drop * smoothEnds * ease;
        if (cut <= 0) continue;
        double &ground = terrainElevation_[y * resolution + x];
        ground = std::max(seaLevelMetres + 1.0, ground - cut);
        // The caprock: hardest at the brink and falling away over the face, so
        // the solver keeps the lip sharp while freely working the debris below.
        if (!rockHardness_.empty())
        {
          const double band = std::exp(-(best / std::max(1.0, face * 1.5)) *
              (best / std::max(1.0, face * 1.5)));
          double &rock = rockHardness_[y * resolution + x];
          rock = std::max(rock, 1.0 + (scarps.resistance - 1.0) * band * smoothEnds);
        }
      }
    scarpLines_.push_back({line, drop, scarps.faceMetres, runout});
    ++made;
  }
}

// Weather the scarps at raster resolution.
//
// The stream-power solver cannot do this. It runs at `maximumResolution` -
// 250 m cells on a 256 km map - and a cliff face is one raster cell at 125 m,
// so the solver cannot represent one, let alone erode one: measured, cutting
// before the solver changed the slope distribution not at all, with or without
// a caprock band. Widening the cut so the solver *could* see it made the
// terrain gentler, because a drop spread wide enough to resolve at 250 m is a
// ramp by definition.
//
// So the scarps are cut after the solver, into ground the solver has already
// shaped - they inherit its drainage and its irregular contours - and are then
// weathered here by the one process that acts at the scale of a cliff: material
// standing steeper than its angle of repose slides, and piles up as talus at
// the foot. This is the same rule the solver's landslide pass applies, run on
// the fine grid where a cliff actually exists.
void TerrainMapSource::sharpenScarps()
{
  if (scarpLines_.empty() || scarps.weatherRounds == 0u) return;
  const double cell = cellMetres();
  const double repose = std::tan(std::clamp(scarps.reposeDegrees, 5.0, 60.0) * 3.14159265359 / 180.0);
  const double limit = repose * cell;
  // Only the ground around a scarp is touched, so the rest of the world keeps
  // the surface the solver produced.
  std::vector<uint8_t> active(terrainElevation_.size(), 0u);
  for (const auto &scarp : scarpLines_)
    for (const Point2 &p : scarp.path)
    {
      const auto reach = size_t(std::ceil((scarp.runout * .35) / cell));
      const auto cx = int64_t(p.x / cell), cy = int64_t(p.y / cell);
      for (int64_t y = cy - int64_t(reach); y <= cy + int64_t(reach); ++y)
        for (int64_t x = cx - int64_t(reach); x <= cx + int64_t(reach); ++x)
          if (x >= 0 && y >= 0 && x < int64_t(resolution) && y < int64_t(resolution))
            active[size_t(y) * resolution + size_t(x)] = 1u;
    }
  std::vector<double> delta(terrainElevation_.size());
  for (uint32_t round = 0; round < scarps.weatherRounds; ++round)
  {
    std::fill(delta.begin(), delta.end(), 0.0);
    for (size_t y = 1; y + 1 < resolution; ++y)
      for (size_t x = 1; x + 1 < resolution; ++x)
      {
        const size_t id = y * resolution + x;
        if (!active[id]) continue;
        const double here = terrainElevation_[id];
        // Everything this cell stands above by more than the repose step is
        // unstable; hand that excess to the neighbours below, in proportion.
        double excess = 0;
        double share[4] = {0, 0, 0, 0};
        const size_t around[4] = {id - 1, id + 1, id - resolution, id + resolution};
        for (int k = 0; k < 4; ++k)
        {
          const double fall = here - terrainElevation_[around[k]];
          if (fall > limit) { share[k] = fall - limit; excess += share[k]; }
        }
        if (!(excess > 0)) continue;
        const double move = excess * scarps.weatherRate / 4.0;
        delta[id] -= move;
        for (int k = 0; k < 4; ++k)
          if (share[k] > 0) delta[around[k]] += move * (share[k] / excess);
      }
    for (size_t id = 0; id < terrainElevation_.size(); ++id) terrainElevation_[id] += delta[id];
  }
}
} // namespace procedural::geometry
