#include "procedural/world/generator/TownLayout.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace procedural::world;
using namespace procedural::world::generator;

namespace {

int failures = 0, assertions = 0;

void check(bool ok, const std::string &what)
{
  ++assertions;

  if (!ok)
  {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
  else
  {
    std::printf("PASS %s\n", what.c_str());
  }
}

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

// A block is the land between streets now, which is usually concave, so
// containment is the even-odd rule and not the convex half-plane test.
bool insideOrOn(const ConvexRing &ring, Position p, double tolerance)
{
  return strictlyInsideConvex(ring, p) || insideEvenOdd(ring, p) || onRing(ring, p, tolerance);
}

// A 6 x 6 grid of 40 m cells: the central sixteen are the walled core (one
// the plaza), the twenty around them the outskirts. The border is the core's
// circumference with every cell corner on it (where two core cells meet).
TownLayoutInput town()
{
  TownLayoutInput in;
  uint64_t id = 1;

  for (int row = 0; row < 6; ++row)
  {
    for (int column = 0; column < 6; ++column)
    {
      TownPatch patch;
      patch.id = id++;
      const double x0 = column * 40., z0 = row * 40.;
      patch.ring = {{x0, 0, z0}, {x0 + 40, 0, z0}, {x0 + 40, 0, z0 + 40}, {x0, 0, z0 + 40}};
      const bool core = row >= 1 && row <= 4 && column >= 1 && column <= 4;
      patch.withinWalls = core;
      patch.outskirts = !core;
      patch.plaza = row == 2 && column == 2;
      patch.use = core ? 7 : 8;
      in.patches.push_back(patch);
    }
  }

  for (double x = 40; x < 200; x += 40)
  {
    in.border.push_back({x, 0, 40});
  }

  for (double z = 40; z < 200; z += 40)
  {
    in.border.push_back({200, 0, z});
  }

  for (double x = 200; x > 40; x -= 40)
  {
    in.border.push_back({x, 0, 200});
  }

  for (double z = 200; z > 40; z -= 40)
  {
    in.border.push_back({40, 0, z});
  }

  in.gates = {{120, 0, 40}, {120, 0, 200}};
  in.exits = {{120, 0, -60}, {120, 0, 300}};
  in.centre = {120, 0, 120};
  in.walled = true;
  in.wallSectionMetres = 40;
  in.seed = 3;
  in.random = [](uint64_t id, uint64_t purpose) {
    uint64_t h = (id + 1) * 0x9E3779B97F4A7C15ULL ^ (purpose + 1) * 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 31;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 29;
    return double(h % 1000000) / 1000000.;
  };
  return in;
}

} // namespace

int main()
{
  {
    rewrite::CityStreet street;street.id=1;street.a={-5,0,-2};street.b={35,0,-2};
    street.path={street.a,street.b};street.halfWidth=2;
    std::vector<ConvexRing> plots={{{0,0,0},{24,0,0},{24,0,10},{0,0,10}}};
    const auto changes=TownLayout::packFrontageLots(plots,{street});
    double area=0;for(const auto &p:plots)area+=signedArea(p);
    check(changes>0 && plots.size()>=2 && std::abs(area-240)<.01,
          "spare street frontage divides into additional full-size parcels without losing land");
    bool fronted=true;for(const auto &p:plots) {
      double width=0;for(size_t k=0;k<p.size();++k)
        if(std::abs(p[k].z)<1e-6 && std::abs(p[(k+1)%p.size()].z)<1e-6)
          width+=std::abs(p[k].x-p[(k+1)%p.size()].x);
      fronted &= width>=6.6-1e-6;
    }
    check(fronted,"every remainder house keeps full modular frontage on the original street");
    std::vector<ConvexRing> strips={{{0,0,0},{4,0,0},{4,0,10},{0,0,10}},
                                   {{4,0,0},{16,0,0},{16,0,10},{4,0,10}}};
    check(TownLayout::packFrontageLots(strips,{street})>0 && strips.size()==2,
          "thin parcel borrows neighbouring land and repartitions into two viable homes");
    auto separate=std::vector<ConvexRing>{{{0,0,0},{4,0,0},{4,0,10},{0,0,10}},
                                        {{6,0,0},{16,0,0},{16,0,10},{6,0,10}}};
    check(TownLayout::packFrontageLots(separate,{street})==0,
          "parcel expansion never spans the public gap between neighbours");
    auto backland=std::vector<ConvexRing>{{{0,0,0},{24,0,0},{24,0,10},{0,0,10}}};
    check(TownLayout::packFrontageLots(backland,{})==0 && backland.size()==1,
          "large remainder without street access cannot become speculative houses");
  }

  {
    TownStreetsInput input;
    input.border={{0,0,0},{40,0,0},{40,0,40},{0,0,40}};
    TownPatch patch; patch.id=1; patch.ring=input.border; patch.withinWalls=true;
    input.patches={patch}; input.gates={{0,0,0}}; input.centre={20,0,20};
    input.obstacles={{{-2,0,-2},{5,0,-2},{5,0,5},{-2,0,5}}};
    TownStreetsPlan routes; Ledger ledger; std::string error;
    input.maximumGateRelocation=0;
    check(!TownStreets::plan(input,routes,ledger,&error),"pinned blocked gate reports failure instead of crossing a compound");
    input.maximumGateRelocation=40;
    check(TownStreets::plan(input,routes,ledger,&error) && routes.failed==0 &&
          distance(routes.gates[0],input.gates[0])>0 && distance(routes.gates[0],input.gates[0])<=40 &&
          onRing(input.border,routes.gates[0],1e-6),"blocked gate relocates within its boundary budget");
  }

  {
    const auto base=town(); TownStreetsInput input;
    input.patches=base.patches; input.border=base.border; input.centre=base.centre;
    input.gates={{120,0,200}};
    input.obstacles={{{116,0,116},{124,0,116},{124,0,124},{116,0,124}}};
    TownStreetsPlan routes; Ledger ledger; std::string error;
    check(TownStreets::plan(input,routes,ledger,&error) && routes.streets==1 && routes.failed==0,
          "blocked nearest plaza entrance uses another reachable corner");
  }

  {
    const auto base=town(); TownStreetsInput input;
    input.patches=base.patches; input.border=base.border; input.centre=base.centre;
    input.gates={{120,0,200},{200,0,200}}; input.smoothing=0;
    TownStreetsPlan independent,shared; Ledger ledger; std::string error;
    input.existingStreetCost=1;
    const bool first=TownStreets::plan(input,independent,ledger,&error);
    input.existingStreetCost=.3;
    const bool second=TownStreets::plan(input,shared,ledger,&error);
    const auto length=[](const TownStreetsPlan &r) { double sum=0;for(const auto &line:r.arteries)
      for(size_t i=1;i<line.size();++i)sum+=distance(line[i-1],line[i]);return sum; };
    check(first && second && shared.streets==2 && length(shared)<length(independent)-1,
          "reuse-aware A* serves both gates with less newly built street length");
  }

  {
    TownStreetsInput input;
    input.border={{0,0,0},{100,0,0},{100,0,100},{0,0,100}};
    TownPatch patch; patch.id=1; patch.ring=input.border; patch.withinWalls=true;
    input.patches={patch}; input.gates=input.border; input.centre={50,0,50};
    TownStreetsPlan routes; Ledger ledger; std::string error;
    check(TownStreets::plan(input,routes,ledger,&error) && routes.streets==4 && routes.failed==0 && routes.segments==4,
          "every corner gate can route directly through the cell interior");
  }

  {
    const auto base=town();
    TownStreetsInput input;
    input.patches=base.patches; input.border=base.border;
    input.gates={{120,0,200}}; input.centre=base.centre;
    input.smoothing=3;
    // A narrow ridge lies between gate and plaza, but both endpoints are
    // level. Endpoint-only slope tests would incorrectly cross it.
    input.terrain=[](double x,double z) {
      return x>101 && x<139 && z>141 && z<159 ? 30. : 0.;
    };
    TownStreetsPlan routes,repeat; Ledger ledger; std::string error;
    check(TownStreets::plan(input,routes,ledger,&error) && routes.streets==1,
          "terrain A* finds a route around an interior ridge: "+error);
    bool safe=true,detour=false;
    for(const auto &line:routes.arteries)for(size_t i=1;i<line.size();++i) {
      auto a=line[i-1],b=line[i]; const unsigned n=std::max(2u,unsigned(std::ceil(distance(a,b)/1.)));
      for(unsigned j=0;j<=n;++j) {
        const double t=double(j)/n,x=a.x+(b.x-a.x)*t,z=a.z+(b.z-a.z)*t;
        safe &= input.terrain(x,z)==0;
        detour |= z>140 && z<160 && std::abs(x-120)>19;
      }
    }
    check(safe && detour,"sampled ridge remains clear after street relaxation");
    TownStreets::plan(input,repeat,ledger,&error);
    bool same=routes.arteries.size()==repeat.arteries.size();
    for(size_t i=0;same && i<routes.arteries.size();++i) {
      same &= routes.arteries[i].size()==repeat.arteries[i].size();
      for(size_t j=0;same && j<routes.arteries[i].size();++j)
        same &= distance(routes.arteries[i][j],repeat.arteries[i][j])<1e-9;
    }
    check(same,"terrain routing and reuse are deterministic");
    input.terrain={};
    input.obstacles={{{101,0,141},{139,0,141},{139,0,159},{101,0,159}}};
    check(TownStreets::plan(input,repeat,ledger,&error),"reserved compound routes around the same corridor");
    bool clear=true;
    for(const auto &line:repeat.arteries)for(size_t i=1;i<line.size();++i)
      for(unsigned j=0;j<=100;++j) {
        const double t=j/100.; auto a=line[i-1],b=line[i];
        clear &= !strictlyInsideConvex(input.obstacles[0],{a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t});
      }
    check(clear,"reserved compound stays clear after smoothing");
    input.existingStreetCost=0;
    check(!TownStreets::plan(input,repeat,ledger,&error),"zero reuse cost is rejected before A*");
  }

  {
    TownBlocksInput input;
    input.random=town().random;
    TownPatch patch;
    patch.id=900; patch.withinWalls=true; patch.use=uint8_t(rewrite::DistrictUse::Residential);
    patch.ring={{0,0,0},{160,0,0},{160,0,120},{0,0,120}};
    input.patches={patch};
    TownBlocksPlan blocks,again;
    Ledger ledger; std::string error;
    check(TownBlocks::plan(input,blocks,ledger,&error),"large residential patch subdivides: "+error);
    bool bounded=true,interiorStreet=false;
    double covered=0;
    for (const auto &b:blocks.blocks) {
      covered+=signedArea(b.ring);
      bounded &= signedArea(b.ring)<=input.maximumInteriorBlockArea;
      for (auto p:b.ring) bounded &= insideOrOn(patch.ring,p,1e-6);
    }
    for (const auto &l:blocks.lanes)
      interiorStreet |= strictlyInsideConvex(patch.ring,{(l.a.x+l.b.x)*.5,0,(l.a.z+l.b.z)*.5});
    check(blocks.blocks.size()>8 && interiorStreet && bounded && covered>signedArea(patch.ring)*.7,
          "large core gets interior street frontage and compact blocks without wasting most of its land");
    TownBlocks::plan(input,again,ledger,&error);
    check(again.blocks.size()==blocks.blocks.size() && again.lanes.size()==blocks.lanes.size(),"density refinement is deterministic");
    input.patches[0].plaza=true;
    TownBlocks::plan(input,again,ledger,&error);
    check(!again.blocks.empty() && std::any_of(again.lanes.begin(),again.lanes.end(),[](const auto &lane){return lane.arterial;}),
          "central routing cell becomes developed blocks with a promoted internal commercial street");
    input.patches[0].plaza=false; input.compounds={patch.ring};
    TownBlocks::plan(input,again,ledger,&error);
    check(again.blocks.empty(),"reserved compound is not densified");
  }
  TownLayoutInput in = town();
  rewrite::CityLayout layout;
  Ledger ledger;
  std::string error;
  const bool ok = TownLayout::plan(in, layout, ledger, &error);
  check(ok, "town layout plans a walled grid town: " + error);

  if (ok)
  {
    // Streets: both gates reach the plaza inside, and roads leave outward.
    size_t inside = 0, outside = 0;

    for (const auto &street : layout.streets)
    {
      const Position m{(street.a.x + street.b.x) * .5, 0, (street.a.z + street.b.z) * .5};
      const bool in = m.x > 40 && m.x < 200 && m.z > 40 && m.z < 200;
      (in ? inside : outside)++;
    }

    check(inside >= 2 && outside >= 2, "streets run from both gates to the plaza and roads leave both gates");

    for (const auto &street : layout.streets)
    {
      check(
          !(street.a.x > 40 && street.a.x < 120 && street.a.z > 40 && street.a.z < 120 && street.b.x > 40 && street.b.x < 120 && street.b.z > 40 && street.b.z < 120 && std::abs(street.a.x - 80) < 1e-6 &&
            std::abs(street.b.x - 80) < 1e-6 && street.a.z > 40 && street.a.z < 80 && street.b.z > 40 && street.b.z < 80),
          "no segment runs along the plaza's own edge");
    }

    // Wall: sections on the circumference, opened at the gates, towers off the gates.
    check(layout.walls.size() == in.border.size(), "one wall section per border edge");
    bool openedAtGates = true;

    for (const auto &wall : layout.walls)
    {
      for (auto gate : in.gates)
      {
        openedAtGates &= distance(wall.a, gate) > 3 && distance(wall.b, gate) > 3;
      }
    }

    check(openedAtGates, "wall sections stop short of the gates");
    bool towersOffGates = !layout.towers.empty();

    for (const auto &tower : layout.towers)
    {
      for (auto gate : in.gates)
      {
        towersOffGates &= distance(tower.center, gate) > 1;
      }
    }

    check(towersOffGates, "towers stand on the wall but never on a gate");

    // Blocks: one per patch but the plaza, each inside its patch; buildings
    // inside their block, four corners or more, none on a street.
    check(layout.blocks.size() >= in.patches.size(), "central anchor is developed as a normal commercial block");
    bool blocksInside = true, buildingsInside = true, noTriangles = true, offStreets = true, anyBuilding = false, anyOuter = false;

    // Against the WARD the layout published, not the patch it was given: the
    // street relaxation moves the shared vertices and the patches follow it,
    // which is the whole point of smoothing a street. A block still may not
    // leave the ward it belongs to.
    for (const auto &block : layout.blocks)
    {
      // A block is the land between streets, so it spans as many cells as
      // the streets leave joined: the claim is that it stays on built-up
      // land, vertex by vertex, not that one cell contains it.
      for (auto v : block.boundary.outer)
      {
        bool onWard = false;

        for (const auto &w : layout.wards)
        {
          for (const auto &piece : w.pieces)
          {
            onWard = onWard || insideOrOn(piece, v, .5);
          }
        }

        blocksInside &= onWard;
      }

      for (const auto &lot : block.lots)
      {
        anyBuilding = true;
        anyOuter |= block.use == rewrite::DistrictUse::Suburb;
        noTriangles &= lot.size() >= 4;

        for (auto v : lot)
        {
          buildingsInside &= insideOrOn(block.boundary.outer, v, 1e-6);

          for (const auto &street : layout.streets)
          {
            offStreets &= segmentDistance(v, street.a, street.b) >= street.halfWidth - 1e-6;
          }
        }
      }
    }

    check(blocksInside, "blocks lie on the town's own land");
    check(anyBuilding && buildingsInside, "buildings lie within their blocks");
    check(noTriangles, "no building is a triangle");
    check(offStreets, "no building reaches into a street");
    check(anyOuter, "the outskirts keep buildings along their roads");
    check(!layout.contours.empty() && !layout.wards.empty() && !layout.suburbs.empty(), "ground, wards and suburbs are published for the world builder");
  }

  {
    auto input=town();input.villageFarmsteads=true;
    rewrite::CityLayout layout;Ledger ledger;std::string error;
    const bool planned=TownLayout::plan(input,layout,ledger,&error);
    const auto area=[](const auto &ring){double sum=0;for(size_t i=0;i<ring.size();++i){auto a=ring[i],b=ring[(i+1)%ring.size()];sum+=a.x*b.z-b.x*a.z;}return std::abs(sum)*.5;};
    double core=0,lots=0;
    for(const auto &block:layout.blocks)if(block.use!=rewrite::DistrictUse::Suburb) {
      core+=area(block.boundary.outer);for(const auto &lot:block.lots)lots+=area(lot);
    }
    check(planned && core>0 && lots/core>.70,"enclosed core retains high lot occupancy under the village farmstead policy: "+error);
  }

  for(uint64_t seed:{17,41,91})for(double scale:{.65,1.,1.4}) {
    auto input=town();input.seed=seed;
    const auto random=input.random;input.random=[=](uint64_t id,uint64_t purpose){return random(id^seed,purpose);};
    const auto resize=[&](Position &p){p.x*=scale;p.z*=scale;};
    for(auto &p:input.patches)for(auto &v:p.ring)resize(v);
    for(auto &v:input.border)resize(v);for(auto &v:input.gates)resize(v);for(auto &v:input.exits)resize(v);resize(input.centre);
    rewrite::CityLayout layout;Ledger ledger;std::string error;
    const bool planned=TownLayout::plan(input,layout,ledger,&error);
    bool clear=planned;size_t lots=0;
    for(const auto &block:layout.blocks)for(const auto &lot:block.lots){++lots;for(auto v:lot)for(const auto &street:layout.streets)
      {const double gap=segmentDistance(v,street.a,street.b)-street.halfWidth;
       if(gap< -1e-6 && clear)std::printf("clearance seed=%llu scale=%g gap=%g street=%llu width=%g at %g,%g road %g,%g to %g,%g\n",(unsigned long long)seed,scale,gap,(unsigned long long)street.id,street.halfWidth,v.x,v.z,street.a.x,street.a.z,street.b.x,street.b.z);
       clear &= gap>=-1e-6;
      }
    }
    check(clear && lots>0,"multi-seed street clearance seed="+std::to_string(seed)+" scale="+std::to_string(scale)+": "+error);
  }

  std::printf("TOWN_TESTS assertions=%d failures=%d\n", assertions, failures);
  return failures == 0 ? 0 : 1;
}
