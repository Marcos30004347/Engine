#include "CityMeshInstances.hpp"
#include "procedural/world/WorldCityWaterways.hpp"
#include "procedural/world/WorldCityBase.hpp"
#include "procedural/world/WorldCityRelease.hpp"
#include "procedural/world/WorldCityDistrictUpgrade.hpp"
// Renders the actual-world settlement integration (W0) as SVG/HTML/JSON:
// the world overview with every evaluated site candidate, and the local
// settlement domain with suitability, regions, districts, farmland, exits
// and gates. Vector output is the review evidence; PNGs are rasterised
// from the SVGs by the caller.
#include "json.hpp"
#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/MapExport.hpp"
#include "procedural/world/WorldSettlementFabricAdapter.hpp"
#include "procedural/world/WorldKingdomHistory.hpp"
#include "CityHistoryJson.hpp"
#include "WorldStoryJson.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"
#include "procedural/world/citywfc/CityGrid.hpp"
#include "procedural/world/citywfc/CityPlan.hpp"
#include "procedural/world/generator/BlockGrammar.hpp"
#include "procedural/world/generator/CityBlockPlanner.hpp"
#include "procedural/world/generator/SettlementGenerators.hpp"
#include "procedural/world/generator/WorldRoads.hpp"
#include "procedural/world/hierarchy/DwellingVoxelAssembly.hpp"
#include "editor/virtualgeometry/CityVoxelDwellingCompiler.hpp"
#include "voxel/svraster/CityGroundOverlay.hpp"
#include "editor/virtualgeometry/CityVoxelTerrainTileSampler.hpp"
#include "editor/virtualgeometry/CityVoxelCityAssembler.hpp"
#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "editor/virtualgeometry/CityVoxelMeshVoxelizer.hpp"
#include <cmath>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <set>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
using namespace procedural::world;
using namespace procedural::world::rewrite;
using nlohmann::json;

namespace {

// Colour of a city-mesh triangle for the voxel export, 0xAABBGGRR as the
// voxel decoders read it. A face's own surface (the texture manifest's
// colour, assets/procedural/textures/medieval-pixel/materials.json) wins;
// otherwise the material palette tools/terrain/render_city_model.py draws.
uint32_t cityMeshVoxelColour(uint32_t material,uint32_t surface) {
  static const std::map<uint32_t,std::array<uint8_t,3>> surfaces{
      {1,{205,205,201}},{2,{190,106,70}},{3,{115,79,48}},{4,{224,210,172}},{5,{100,147,159}},
      {6,{164,158,145}},{7,{70,73,77}},{8,{140,91,48}},{9,{208,200,181}},{10,{115,79,48}},
      {11,{91,106,54}},{12,{134,101,67}},{13,{130,127,116}},{14,{182,174,151}},{15,{150,151,146}},
      {16,{198,188,164}},{17,{122,92,58}},{18,{130,127,116}},{19,{130,127,116}},{20,{55,58,64}},
      {22,{126,139,113}},{23,{74,153,167}},{24,{64,104,54}},{25,{190,106,70}},{26,{224,210,172}},
      {27,{116,117,110}},{28,{48,111,119}},{29,{224,166,89}},{30,{190,103,67}},{31,{134,174,132}},
      {32,{170,116,78}},{33,{204,194,168}}};
  static const std::map<uint32_t,std::array<uint8_t,3>> materials{
      {12,{214,194,156}},{13,{151,116,93}},{14,{143,63,50}},{15,{181,164,132}},{16,{91,126,145}},
      {17,{105,78,55}},{18,{174,166,137}},{31,{119,116,109}},{32,{170,165,154}},{33,{104,104,100}},
      {34,{112,64,54}},{35,{138,129,117}},{36,{112,91,85}},{37,{169,142,125}},{38,{103,83,79}},
      {39,{111,48,44}},{40,{182,168,144}},{41,{145,117,95}},{42,{157,75,56}},{43,{180,162,129}},
      {44,{84,121,140}},{45,{110,85,66}},{46,{173,164,135}},{50,{200,188,164}},{51,{139,139,133}},
      {52,{157,133,101}},{53,{68,72,76}},{54,{146,141,130}},{55,{105,78,55}}};
  std::array<uint8_t,3> rgb{183,170,145};
  if(const auto found=surfaces.find(surface);found!=surfaces.end())rgb=found->second;
  else if(const auto found=materials.find(material);found!=materials.end())rgb=found->second;
  return 0xff000000u|uint32_t(rgb[2])<<16|uint32_t(rgb[1])<<8|uint32_t(rgb[0]);
}
using Poly = std::vector<Position>;

std::string biomeColour(Biome b)
{
  switch (b)
  {
  case Biome::Ocean:
    return "#2f5f9e";
  case Biome::Lake:
    return "#4d8fd1";
  case Biome::Shore:
    return "#d8c89a";
  case Biome::Glacier:
    return "#f0f4f8";
  case Biome::Tundra:
    return "#b9c2b0";
  case Biome::ColdDesert:
    return "#c9c0a8";
  case Biome::SaltFlat:
    return "#e8e4d6";
  case Biome::Taiga:
    return "#3f6b4a";
  case Biome::TemperateForest:
    return "#4f8a45";
  case Biome::TemperateRainforest:
    return "#2f7a4a";
  case Biome::MontaneForest:
    return "#5f7f55";
  case Biome::Mediterranean:
    return "#9aa552";
  case Biome::Grassland:
    return "#a8c66c";
  case Biome::Steppe:
    return "#c6b96a";
  case Biome::Shrubland:
    return "#b6a25e";
  case Biome::Savanna:
    return "#d1b862";
  case Biome::TropicalSeasonalForest:
    return "#5c9a3c";
  case Biome::TropicalRainforest:
    return "#2e7a2e";
  case Biome::SubtropicalForest:
    return "#3f8f4f";
  case Biome::Desert:
    return "#e0c48a";
  case Biome::SandDesert:
    return "#ead9a2";
  case Biome::Wetland:
    return "#7aa87a";
  case Biome::Swamp:
    return "#5f7f5f";
  case Biome::Mangrove:
    return "#4f7f6f";
  case Biome::AlpineMeadow:
    return "#9fc08a";
  case Biome::BareRock:
    return "#8f8a82";
  default:
    return "#888";
  }
}

std::string useColour(DistrictUse u)
{
  switch (u)
  {
  case DistrictUse::Residential:
    return "#e07b5f";
  case DistrictUse::Craftsmen:
    return "#d9a441";
  case DistrictUse::WorkerHousing:
    return "#c56f9a";
  case DistrictUse::Temple:
    return "#8e6bd6";
  case DistrictUse::Civic:
    return "#4a8fd6";
  case DistrictUse::Merchant:
    return "#e2b04a";
  case DistrictUse::Military:
    return "#7a7a7a";
  case DistrictUse::Suburb:
    return "#f0a080";
  case DistrictUse::WealthyResidential:
    return "#f2c14e";
  default:
    return "#999";
  }
}

struct Svg
{
  std::ostringstream out;
  double scale = 1, width = 0, height = 0;

  Svg(double w, double h, double s) : scale(s), width(w), height(h)
  {
    out << std::fixed << std::setprecision(2) << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << " " << h << "\">\n";
  }

  double x(double v) const
  {
    return v * scale;
  }

  double y(double v) const
  {
    return height - v * scale;
  }

  void polygon(const Poly &p, const std::string &fill, const std::string &stroke, double strokeWidth, double opacity = 1)
  {
    if (p.size() < 3)
    {
      return;
    }
    out << "<polygon points=\"";
    for (auto q : p)
    {
      out << x(q.x) << "," << y(q.z) << " ";
    }
    out << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << strokeWidth << "\" fill-opacity=\"" << opacity << "\"/>\n";
  }

  void polyline(const Poly &p, const std::string &stroke, double strokeWidth, const std::string &extra = "")
  {
    if (p.size() < 2)
    {
      return;
    }
    out << "<polyline points=\"";
    for (auto q : p)
    {
      out << x(q.x) << "," << y(q.z) << " ";
    }
    out << "\" fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\"" << strokeWidth << "\" " << extra << "/>\n";
  }

  void rect(double x0, double z0, double w, double h, const std::string &fill, double opacity = 1)
  {
    out << "<rect x=\"" << x(x0) << "\" y=\"" << y(z0 + h) << "\" width=\"" << w * scale << "\" height=\"" << h * scale << "\" fill=\"" << fill << "\" fill-opacity=\"" << opacity << "\"/>\n";
  }

  void circle(Position p, double r, const std::string &fill, const std::string &stroke, double strokeWidth, double opacity = 1)
  {
    out << "<circle cx=\"" << x(p.x) << "\" cy=\"" << y(p.z) << "\" r=\"" << r << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << strokeWidth << "\" fill-opacity=\"" << opacity << "\"/>\n";
  }

  void text(Position p, const std::string &s, double size, const std::string &fill = "#111", const std::string &anchor = "middle")
  {
    out << "<text x=\"" << x(p.x) << "\" y=\"" << y(p.z) << "\" font-size=\"" << size << "\" font-family=\"Helvetica, Arial, sans-serif\" fill=\"" << fill << "\" text-anchor=\"" << anchor << "\">" << s << "</text>\n";
  }

  std::string finish()
  {
    out << "</svg>\n";
    return out.str();
  }
};

double signedAreaOf(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return a * .5;
}

Position centroid(const Poly &p)
{
  Position c;
  for (auto q : p)
  {
    c.x += q.x / p.size();
    c.z += q.z / p.size();
  }
  return c;
}

json poly(const Poly &p)
{
  json a = json::array();
  for (auto v : p)
  {
    a.push_back({v.x, v.y, v.z});
  }
  return a;
}

// Marching squares over a regular grid of heights (cell centres).
std::vector<Poly> contourLines(const std::vector<double> &h, uint32_t cols, uint32_t rows, double x0, double z0, double spacing, double level)
{
  std::vector<Poly> lines;
  auto at = [&](uint32_t i, uint32_t j) {
    return h[j * cols + i];
  };
  auto pos = [&](double i, double j) {
    return Position{x0 + (i + .5) * spacing, 0, z0 + (j + .5) * spacing};
  };
  for (uint32_t j = 0; j + 1 < rows; ++j)
  {
    for (uint32_t i = 0; i + 1 < cols; ++i)
    {
      double v[4] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
      Position c[4] = {pos(i, j), pos(i + 1, j), pos(i + 1, j + 1), pos(i, j + 1)};
      std::vector<Position> cuts;
      for (int k = 0; k < 4; ++k)
      {
        double a = v[k], b = v[(k + 1) % 4];
        if ((a < level) != (b < level))
        {
          double t = (level - a) / (b - a);
          cuts.push_back({c[k].x + (c[(k + 1) % 4].x - c[k].x) * t, 0, c[k].z + (c[(k + 1) % 4].z - c[k].z) * t});
        }
      }
      if (cuts.size() == 2)
      {
        lines.push_back({cuts[0], cuts[1]});
      }
      else if (cuts.size() == 4)
      {
        lines.push_back({cuts[0], cuts[1]});
        lines.push_back({cuts[2], cuts[3]});
      }
    }
  }
  return lines;
}
} // namespace

// ---- one block, end to end ------------------------------------------------
//
// The whole city takes four minutes a run, which is not a loop anybody can
// design in. A block is the unit the planner actually reasons about, and one
// of them builds in seconds: boundary, pavement, frontage rows, the way into
// the middle and the garden it leads to, then the houses. Everything the
// block layer grows is developed here first and only then let loose on a
// city.
int planOneBlock(int argc, char **argv)
{
  const auto ringArea=[](const std::vector<Position> &r){
    if(r.size()<3)return 0.;
    double twice=0;
    for(size_t i=0,j=r.size()-1;i<r.size();j=i++)twice+=r[j].x*r[i].z-r[i].x*r[j].z;
    return std::abs(twice)*.5;
  };

  double width=46,depth=34,skew=0;
  uint64_t seed=7;
  int streets=4;
  bool plaza=false, infrastructure=false;
  auto use=DistrictUse::Residential;
  std::filesystem::path output="build/block";

  for(int i=2;i+1<argc;i+=2) {
    const std::string option=argv[i],value=argv[i+1];
    if(option=="--width")width=std::stod(value);
    else if(option=="--depth")depth=std::stod(value);
    else if(option=="--skew")skew=std::stod(value);
    else if(option=="--seed")seed=std::stoull(value);
    else if(option=="--streets")streets=std::stoi(value);
    else if(option=="--plaza")plaza=std::stoi(value)!=0;
    else if(option=="--infrastructure")infrastructure=std::stoi(value)!=0;
    else if(option=="--output")output=value;
    else if(option=="--use")
      use = value=="merchant" ? DistrictUse::Merchant
          : value=="craftsmen" ? DistrictUse::Craftsmen
          : value=="suburb" ? DistrictUse::Suburb
          : value=="worker" ? DistrictUse::WorkerHousing
          : value=="wealthy" ? DistrictUse::WealthyResidential
          : DistrictUse::Residential;
  }

  // A block, skewed if asked: a real one is rarely a rectangle.
  generator::CityBlockPlanInput in;
  in.id=1; in.seed=seed; in.use=use;
  // A suburb is crofts, not a thinner terrace.
  in.plaza = plaza;
  in.crofts = !plaza && (use==DistrictUse::Suburb || use==DistrictUse::WorkerHousing);
  if(in.crofts) {
    in.lotFrontageMetres=in.croftFrontageMetres;
    in.lotDepthMetres=in.toftDepthMetres+in.yardDepthMetres+in.penDepthMetres+7.;
    in.minimumLotMetres2=70.;
    in.sidewalkMetres=.9;
  }
  in.boundary={{0,0,0},{width,0,0},{width+skew,0,depth},{skew*.4,0,depth}};
  in.streetEdges={streets>=1,streets>=2,streets>=3,streets>=4};

  generator::CityBlockPlan plan;
  std::string error;
  if(!generator::planCityBlock(in,plan,&error)){std::cerr<<"block: "<<error<<"\n";return 1;}

  std::map<std::string,uint32_t> roles;
  for(const auto &lot:plan.lots)roles[generator::nameOf(lot.role)]++;
  std::cout<<"block "<<width<<" x "<<depth<<" skew "<<skew<<": "<<plan.lots.size()<<" lots (";
  for(const auto &[name,count]:roles)std::cout<<count<<" "<<name<<" ";
  std::cout<<"), "<<plan.sidewalks.size()<<" sidewalks, "<<plan.corridors.size()<<" corridors, "
           <<plan.gardens.size()<<" gardens\n";
  for(const auto &note:plan.notes)std::cout<<"  block: "<<note<<"\n";

  // Into the city model, through the same path a real block takes.
  CityCompletionPlan city;city.revision=1;
  const auto ring=[](const std::vector<Position> &r){return r;};
  city.skeleton.regions.push_back({100,100,0,CityRegionKind::Block,CityLandUse::UrbanBlock,use,ring(in.boundary),false,width*depth});

  city.skeleton.alleys=plan.corridors;
  for(const auto &walk:plan.sidewalks)
    city.skeleton.publicSpaces.push_back({100,CityPublicSpace::Kind::Sidewalk,walk,0});
  for(const auto &court:plan.paving)
    city.skeleton.publicSpaces.push_back({100,CityPublicSpace::Kind::Court,court,plaza?0:.54});
  if(plaza) {
    city.skeleton.plazas.push_back(plan.plaza);
    for(const auto &note:plan.plaza.notes)std::cout<<"  plaza: "<<note<<"\n";
  }

  uint64_t next=200;

  // The houses behind the arcade, developed the way completion develops a
  // planned block lot from its role: rows of shop-houses, an inn, a pub.
  for(const auto &house:plan.plaza.lots) {
    city.skeleton.regions.push_back({next,next,100,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Merchant,ring(house.ring),false,ringArea(house.ring)});
    city.skeleton.regions.back().courtFacing=true;
    city.skeleton.regions.back().publicFrontage={house.ring[2],house.ring[3]};
    CityLotDevelopment lot;lot.lot=next;lot.developed=true;
    lot.rowHouse=house.attached;lot.residence=CityResidenceKind::House;
    lot.merchantHouse=house.role=="shop-house";
    lot.preferredFootprintMetres2=std::min(ringArea(house.ring)*.94,160.);
    lot.desiredRooms=ringArea(house.ring)>=75?5:3;lot.minimumFloors=2;lot.maximumFloors=3;
    if(house.role=="inn" || house.role=="pub") {
      lot.residence=house.role=="inn"?CityResidenceKind::Inn:CityResidenceKind::Pub;
      lot.desiredRooms=house.role=="inn"?14:5;
    } else if(house.role=="bakehouse") {
      lot.desiredRooms=2;lot.minimumFloors=1;lot.maximumFloors=1;lot.groundCover=CityGroundCover::Dirt;
    } else if(house.role=="civic-hall") {
      lot.residence=CityResidenceKind::Mansion;lot.desiredRooms=10;lot.minimumFloors=3;lot.maximumFloors=3;
      lot.preferredFootprintMetres2=ringArea(house.ring)*.9;
    }
    lot.groundCover=lot.groundCover==CityGroundCover::Automatic?CityGroundCover::Stone:lot.groundCover;
    city.lots.push_back(lot);
    ++next;
  }
  uint64_t land=9000;

  // A shambles bay: one trade, one room deep, glued to the next.
  for(const auto &stall:plan.stalls) {
    city.skeleton.regions.push_back({next,next,100,CityRegionKind::Lot,CityLandUse::UrbanLot,use,ring(stall),false,ringArea(stall)});
    CityLotDevelopment bay;bay.lot=next;bay.developed=true;
    bay.rowHouse=true;            // Glued: a shambles is one building with many doors.
    bay.merchantHouse=true;
    bay.desiredRooms=2;
    bay.minimumFloors=1;bay.maximumFloors=2;
    bay.preferredFootprintMetres2=std::max(18.,ringArea(stall)*.9);
    bay.preferredRoomMetres2=std::max(12.,ringArea(stall)*.45);
    bay.groundCover=CityGroundCover::Stone;
    city.lots.push_back(bay);
    ++next;
  }

  // The hall: the biggest roof on the square.
  for(const auto &hall:plan.halls) {
    city.skeleton.regions.push_back({next,next,100,CityRegionKind::Lot,CityLandUse::UrbanLot,use,ring(hall),false,ringArea(hall)});
    CityLotDevelopment market;market.lot=next;market.developed=true;
    market.desiredRooms=6;
    market.preferredRoomMetres2=26;
    market.minimumFloors=1;market.maximumFloors=2;
    market.preferredFootprintMetres2=std::max(60.,ringArea(hall)*.92);
    market.groundCover=CityGroundCover::Stone;
    city.lots.push_back(market);
    ++next;
  }
  for(const auto &lot:plan.lots) {
    // A cottage is fitted to its toft; the yard, the pen and the garden are
    // ground in their own right, so something can stand on them and they
    // render as the working land they are rather than as house plot.
    const bool croft=lot.role==generator::CityBlockLotRole::Cottage && lot.toft.size()>=3;
    const auto &plot=croft?lot.toft:lot.boundary;
    const double plotArea=croft?ringArea(lot.toft):lot.area;
    city.skeleton.regions.push_back({next,next,100,CityRegionKind::Lot,CityLandUse::UrbanLot,use,ring(plot),false,plotArea});

    if(croft) {
      // The parts go in as undeveloped LOTS, not as fields.
      //
      // A field inside a block never survives: the surface builder subtracts
      // every piece of block ground from the fields, so a croft's yard laid
      // out as arable came back as nothing and the whole croft rendered as
      // one green sheet. An undeveloped lot is ground with an owner and a
      // cover and no building on it, which is exactly what a yard is.
      const auto working=[&](const std::vector<Position> &r,const CityGroundCover cover){
        if(r.size()<3)return;
        city.skeleton.regions.push_back({land,land,100,CityRegionKind::Lot,CityLandUse::UrbanLot,use,ring(r),false,ringArea(r)});
        CityLotDevelopment ground;
        ground.lot=land;
        ground.developed=false;
        ground.groundCover=cover;
        city.lots.push_back(ground);
        ++land;
      };
      working(lot.yard,CityGroundCover::Dirt);
      working(lot.pen,CityGroundCover::Dirt);
      working(lot.garden,CityGroundCover::Grass);
    }
    // What the role actually means, as a building.
    //
    // A role that only changes a label is not a building type. Each of these
    // asks for a different thing: an inn wants many small chambers over a big
    // ground floor, a pub one large room and a yard, a bakehouse a single
    // tall space for the oven, a smithy a wide floor and no upper storey over
    // the forge, a shop-house trade below and living above.
    CityLotDevelopment allocation;
    allocation.lot=next;
    allocation.developed=true;
    allocation.residence=CityResidenceKind::House;
    allocation.rowHouse=lot.before!=UINT32_MAX || lot.after!=UINT32_MAX;
    allocation.preferredFootprintMetres2=std::clamp(lot.area*.94,34.,160.);
    allocation.desiredRooms=3;
    allocation.maximumFloors=lot.area>=52?2:1;
    allocation.minimumFloors=1;
    allocation.groundCover=CityGroundCover::Stone;

    switch(lot.role) {
      case generator::CityBlockLotRole::ShopHouse:
      case generator::CityBlockLotRole::Passage:
        allocation.merchantHouse=true;
        allocation.desiredRooms=4;
        allocation.minimumFloors=2;
        allocation.maximumFloors=3;
        break;
      case generator::CityBlockLotRole::Inn:
        allocation.residence=CityResidenceKind::Inn;
        allocation.desiredRooms=14;
        allocation.preferredRoomMetres2=18;
        allocation.minimumFloors=2;
        allocation.maximumFloors=4;
        allocation.preferredFootprintMetres2=std::min(lot.area*.85,110.);
        break;
      case generator::CityBlockLotRole::Pub:
        allocation.residence=CityResidenceKind::Pub;
        allocation.desiredRooms=5;
        allocation.preferredRoomMetres2=22;
        allocation.minimumFloors=1;
        allocation.maximumFloors=2;
        break;
      case generator::CityBlockLotRole::Bakehouse:
        // One tall space and the oven in it; no household over the fire.
        allocation.desiredRooms=2;
        allocation.preferredRoomMetres2=26;
        allocation.minimumFloors=1;
        allocation.maximumFloors=1;
        break;
      case generator::CityBlockLotRole::Smithy:
      case generator::CityBlockLotRole::Workshop:
        allocation.desiredRooms=3;
        allocation.preferredRoomMetres2=24;
        allocation.minimumFloors=1;
        allocation.maximumFloors=2;
        allocation.groundCover=CityGroundCover::Dirt;
        break;
      case generator::CityBlockLotRole::Cottage:
        // The cottage is the built end of the strip, not the strip. Asking
        // for a house the size of the croft is what turns a peasant holding
        // into a manor sitting in a field.
        allocation.rowHouse=false;
        allocation.desiredRooms=2;
        allocation.preferredRoomMetres2=14;
        allocation.minimumFloors=1;
        allocation.maximumFloors=1;
        allocation.preferredFootprintMetres2=std::clamp(ringArea(lot.toft)*.55,26.,48.);
        allocation.groundCover=CityGroundCover::Dirt;
        break;
      case generator::CityBlockLotRole::Dwelling:
        break;
    }
    city.lots.push_back(allocation);
    ++next;
  }

  // The streets the block fronts, so frontage and doors have something to face.
  uint64_t route=1;
  for(size_t i=0;i<in.boundary.size();++i) {
    if(!in.streetEdges[i])continue;
    const auto a=in.boundary[i],b=in.boundary[(i+1)%in.boundary.size()];
    const double dx=b.x-a.x,dz=b.z-a.z,len=std::hypot(dx,dz);
    if(len<1e-6)continue;
    const double nx=dz/len*2.6,nz=-dx/len*2.6; // Outward: the carriageway sits off the block.
    CitySmoothedRoute way;way.id=route++;way.halfWidth=2.4;way.groundCover=CityGroundCover::Stone;
    way.path={{a.x+nx,0,a.z+nz},{b.x+nx,0,b.z+nz}};
    city.routes.push_back(way);
  }

  CityModelPlan model;SettlementFabric fabric;CityModelSettings options;
  options.groundVertexSpacing=1.2;
  options.groundOnly=infrastructure;
  // A croft is fenced. That is what a croft IS: the fence is what keeps the
  // pig in and the neighbour's pig out, and a toft without one is just a
  // house standing in a field. In the town the fences read as clutter and
  // stay off.
  options.lotBoundaries=in.crofts;
  fabric.terraceLevel=0;
  if(!buildPreviewCityModel(city,fabric,options,model,&error)){std::cerr<<"model: "<<error<<"\n";return 1;}
  std::cout<<"block model: "<<model.buildings.size()<<" buildings, "<<model.mesh.triangles.size()<<" triangles\n";

  for(const auto &why:model.rejections)std::cout<<"  rejected: "<<why<<"\n";

  // One block is small, so it goes out as plain JSON: the side-car binary the
  // city uses exists because eleven million triangles will not fit in text,
  // and a block has a few tens of thousands.
  std::filesystem::create_directories(output);
  json j;
  j["sourcePipeline"]="native-block";
  j["vertices"]=json::array();
  for(auto p:model.mesh.vertices)j["vertices"].push_back({p.x,p.y,p.z});
  j["triangles"]=model.mesh.triangles;
  j["materials"]=model.materials;
  j["triangleUVs"]=model.triangleUVs;
  j["surfaceMaterials"]=model.surfaceMaterials;
  j["groundWeights"]=model.groundWeights;
  j["groundMaterialIds"]=model.groundMaterialIds;
  j["textureManifest"]="assets/procedural/textures/medieval-pixel/materials.json";
  j["rejections"]=model.rejections;
  j["gatewayHouses"]=model.gatewayHouses;
  j["buildings"]=json::array();
  for(const auto &b:model.buildings) {
    json footprint=json::array();
    for(auto p:b.footprint)footprint.push_back({p.x,p.y,p.z});
    json doors=json::array();
    for(auto p:b.exteriorDoors)doors.push_back({p.x,p.y,p.z});
    json views=json::array();
    for(auto p:b.interiorViews)views.push_back({p.x,p.y,p.z});
    j["buildings"].push_back({{"id",b.id},{"source",b.source},{"kind",cityModelBuildingKindName(b.kind)},
      {"footprint",footprint},{"base",b.base},{"wallHeight",b.wallHeight},{"roofHeight",b.roofHeight},
      {"exteriorDoors",doors},{"interiorViews",views},{"jettyMetres",b.jettyMetres}});
  }
  writeCityInstances(output/"city-model-c10.json",model,j);std::ofstream(output/"city-model-c10.json")<<j.dump();
  std::cout<<"wrote "<<(output/"city-model-c10.json")<<"\n";
  return 0;
}

static json roomJson(const procedural::world::rewrite::CityModelRoom &room) {
  json boundary=json::array();for(auto p:room.boundary)boundary.push_back({p.x,p.y,p.z});
  return {{"id",room.id},{"floor",room.floor},{"use",room.use},{"kind",room.kind},{"boundary",boundary},{"area",room.area}};
}

// Bounded native fixture: construction ages are authored demonstration inputs.
int planArchitecturalGallery(int argc,char **argv) {
  const std::filesystem::path output=argc>2?argv[2]:"build/phase7/architecture-gallery";
  const bool religious=std::string(argv[1])=="--religious-gallery";
  CityCompletionPlan city;city.revision=1;
  for(unsigned i=0;i<4;++i){const uint64_t id=10+i;const double x=i*38.;
    const double width=i==3 && !religious?20.:32.;
    std::vector<Position> ring={{x,0,0},{x+width,0,0},{x+width,0,32},{x,0,32}};
    city.skeleton.regions.push_back({id,id,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::WealthyResidential,ring,false,1024});
    CityLotDevelopment lot;lot.lot=id;lot.developed=true;lot.desiredRooms=12;
    lot.minimumFloors=lot.maximumFloors=1;lot.preferredFootprintMetres2=760;lot.preferredRoomMetres2=32;
    if(i==3){lot.trade="roman-temple";lot.desiredRooms=2;}
    if(religious){lot.trade=i<2?"church":i==2?"temple":"roman-temple";lot.desiredRooms=4;}
    lot.architecturalAgeYears=i?400:60;lot.groundCover=CityGroundCover::Stone;city.lots.push_back(lot);
  }
  CitySmoothedRoute route;route.id=1;route.halfWidth=2.4;route.path={{-4,0,-4},{150,0,-4}};route.groundCover=CityGroundCover::Stone;city.routes.push_back(route);
  SettlementFabric fabric;fabric.terraceLevel=0;CityModelSettings settings;settings.groundVertexSpacing=2.;
  CityModelPlan model;std::string error;if(!buildPreviewCityModel(city,fabric,settings,model,&error)){std::cerr<<error;return 1;}
  json j;j["sourcePipeline"]="native-block";j["previewPurpose"]="Architectural style fixture with authored construction ages";
  j["vertices"]=json::array();for(auto p:model.mesh.vertices)j["vertices"].push_back({p.x,p.y,p.z});
  j["triangles"]=model.mesh.triangles;j["materials"]=model.materials;j["surfaceMaterials"]=model.surfaceMaterials;
  j["triangleUVs"]=model.triangleUVs;j["groundWeights"]=model.groundWeights;j["groundMaterialIds"]=model.groundMaterialIds;
  j["textureManifest"]="assets/procedural/textures/medieval-pixel/materials.json";j["rejections"]=model.rejections;j["buildings"]=json::array();
  for(const auto &b:model.buildings){json v={{"id",b.id},{"source",b.source},{"kind",cityModelBuildingKindName(b.kind)},
    {"base",b.base},{"wallHeight",b.wallHeight},{"roofHeight",b.roofHeight},{"rooms",b.rooms},{"windows",b.windows},
    {"architecture",b.architecture},{"architecturalAgeYears",b.architecturalAgeYears},{"weathering",b.weathering},
    {"colonnadeColumns",b.colonnadeColumns},{"cofferBeams",b.cofferBeams}};
    v["footprint"]=json::array();for(auto p:b.footprint)v["footprint"].push_back({p.x,p.y,p.z});
    v["exteriorDoors"]=json::array();for(auto p:b.exteriorDoors)v["exteriorDoors"].push_back({p.x,p.y,p.z});
    v["interiorViews"]=json::array();for(auto p:b.interiorViews)v["interiorViews"].push_back({p.x,p.y,p.z});
    v["roomList"]=json::array();for(const auto &r:b.roomList)v["roomList"].push_back(roomJson(r));j["buildings"].push_back(v);
    std::cout<<b.architecture<<" age "<<b.architecturalAgeYears<<": "<<b.rooms<<" rooms, "<<b.colonnadeColumns<<" columns, "<<b.cofferBeams<<" coffer beams\n";
  }
  std::filesystem::create_directories(output);writeCityInstances(output/"city-model-c10.json",model,j);std::ofstream(output/"city-model-c10.json")<<j.dump();
  for(const auto &r:model.rejections)std::cout<<r<<'\n';return model.buildings.size()==4?0:1;
}

int planOneCastle(int argc,char **argv)
{
  double width=140,depth=120;uint64_t seed=41;uint32_t levels=3;std::string style="citadel",keep="auto";uint32_t extraStoreys=0,terraces=0;
  std::filesystem::path output="build/renders/gothic-castle";
  for(int i=2;i+1<argc;i+=2) {
    const std::string key=argv[i],value=argv[i+1];
    if(key=="--width")width=std::stod(value);
    else if(key=="--depth")depth=std::stod(value);
    else if(key=="--seed")seed=std::stoull(value);
    else if(key=="--dungeon-levels")levels=uint32_t(std::stoul(value));
    else if(key=="--keep")keep=value;
    else if(key=="--terraces")terraces=uint32_t(std::stoul(value));
    else if(key=="--extra-storeys")extraStoreys=uint32_t(std::stoul(value));
    else if(key=="--style")style=value;
    else if(key=="--output")output=value;
    else {std::cerr<<"unknown castle option "<<key<<'\n';return 1;}
  }
  generator::CompoundBrief brief;brief.id=700;brief.seed=seed;brief.kind=generator::CompoundKind::Castle;
  brief.fantasyGothic=true;brief.dungeonLevels=levels;brief.extraStoreys=extraStoreys;brief.terraceBands=terraces;
  if(keep=="attached")brief.keepPlacement=generator::KeepPlacement::WallAttached;
  else if(keep=="detached")brief.keepPlacement=generator::KeepPlacement::Detached;
  else if(keep!="auto"){std::cerr<<"keep must be auto, attached or detached";return 1;}
  if(style=="manor")brief.landmarkLayout=generator::LandmarkLayout::GardenManor;
  else if(style=="palace")brief.landmarkLayout=generator::LandmarkLayout::Palace;
  else if(style!="citadel"){std::cerr<<"style must be citadel, manor or palace";return 1;}
  brief.precinct={{0,0,0},{width,0,0},{width,0,depth},{0,0,depth}};brief.approach={width*.5,0,-10};
  generator::CompoundPlan castle;generator::Ledger ledger;std::string error;
  if(!generator::planCompound(brief,[](double,double){return 0.;},castle,ledger,&error)) {std::cerr<<error<<'\n';return 1;}
  SettlementFabric fabric;fabric.terraceLevel=0;fabric.compounds={castle};
  CityCompletionPlan city;city.revision=1;
  city.skeleton.regions.push_back({700,700,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Military,brief.precinct,false,width*depth});
  CityModelPlan model;CityModelSettings settings;settings.groundVertexSpacing=4.;
  if(!buildPreviewCityModel(city,fabric,settings,model,&error)){std::cerr<<error<<'\n';return 1;}
  std::filesystem::create_directories(output);
  const auto write=[&](const CityModelPlan &m,const std::string &name) {
    json j;j["sourcePipeline"]="native-compound";j["vertices"]=json::array();for(auto p:m.mesh.vertices)j["vertices"].push_back({p.x,p.y,p.z});
    j["triangles"]=m.mesh.triangles;j["materials"]=m.materials;j["surfaceMaterials"]=m.surfaceMaterials;
    j["triangleUVs"]=m.triangleUVs;j["groundWeights"]=m.groundWeights;j["groundMaterialIds"]=m.groundMaterialIds;
    j["textureManifest"]="assets/procedural/textures/medieval-pixel/materials.json";
    j["buildings"]=json::array();
    for(const auto &b:m.buildings){json ring=json::array();for(auto p:b.footprint)ring.push_back({p.x,p.y,p.z});
      j["buildings"].push_back({{"id",b.id},{"source",b.source},{"kind",cityModelBuildingKindName(b.kind)},
        {"footprint",ring},{"base",b.base},{"wallHeight",b.wallHeight},{"roofHeight",b.roofHeight},{"rooms",b.rooms},{"windows",b.windows},{"roomConnections",b.roomConnections},{"trade",b.trade}});
      j["buildings"].back()["roomList"]=json::array();for(const auto &room:b.roomList)j["buildings"].back()["roomList"].push_back(roomJson(room));
      j["buildings"].back()["interiorViews"]=json::array();for(auto p:b.interiorViews)j["buildings"].back()["interiorViews"].push_back({p.x,p.y,p.z});
      j["buildings"].back()["exteriorDoors"]=json::array();for(auto p:b.exteriorDoors)j["buildings"].back()["exteriorDoors"].push_back({p.x,p.y,p.z});}
    j["wallAttachedKeep"]=castle.wallAttachedKeep;
    j["terraces"]=json::array();for(const auto &t:castle.terraces){json ring=json::array();for(auto p:t.footprint)ring.push_back({p.x,t.level,p.z});j["terraces"].push_back({{"level",t.level},{"footprint",ring}});}
    j["terraceRoutes"]=json::array();for(const auto &r:castle.paths)j["terraceRoutes"].push_back({{"a",{r.a.x,r.a.y,r.a.z}},{"b",{r.b.x,r.b.y,r.b.z}},{"width",r.width}});
    j["terraceLinks"]=json::array();for(const auto &t:castle.terraceLinks)j["terraceLinks"].push_back({{"from",t.from},{"to",t.to},{"steps",t.steps},{"width",t.width},{"foot",{t.foot.x,t.foot.y,t.foot.z}},{"head",{t.head.x,t.head.y,t.head.z}}});
    j["yards"]=json::array();for(const auto &yard:castle.yards){json ring=json::array();for(auto p:yard.footprint)ring.push_back({p.x,p.y,p.z});j["yards"].push_back({{"role",yard.role},{"footprint",ring}});}
    j["wallAccessLinks"]=json::array();for(const auto &link:m.wallAccessLinks)
      j["wallAccessLinks"].push_back({{"wing",link.wing},{"floor",link.floor},{"clearWidth",link.clearWidth},{"wall",{link.wall.x,link.wall.y,link.wall.z}},{"door",{link.door.x,link.door.y,link.door.z}}});
    j["buildingLinks"]=json::array();for(const auto &link:m.buildingLinks)
      j["buildingLinks"].push_back({{"from",link.from},{"to",link.to},{"width",link.width},{"a",{link.a.x,link.a.y,link.a.z}},{"b",{link.b.x,link.b.y,link.b.z}}});
    j["previewBounds"]=json::array();if(name=="city-model-c10.json")for(auto p:brief.precinct)j["previewBounds"].push_back({p.x,0,p.z});
    j["dungeonRooms"]=json::array();for(const auto &r:castle.dungeonRooms) {
      json ring=json::array();for(auto p:r.footprint)ring.push_back({p.x,r.floor,p.z});
      j["dungeonRooms"].push_back({{"id",r.id},{"level",r.level},{"role",r.role},{"footprint",ring},{"floor",r.floor}});
    }
    j["dungeonConnections"]=json::array();for(const auto &c:castle.dungeonConnections)
      j["dungeonConnections"].push_back({{"from",c.from},{"to",c.to},{"stairs",c.stairs},{"position",{c.position.x,c.position.y,c.position.z}}});
    writeCityInstances(output/name,m,j);std::ofstream(output/name)<<j.dump();
  };
  write(model,"city-model-c10.json");
  for(uint32_t level=0;level<(castle.dungeonRooms.empty()?0:levels);++level) {
    CityModelPlan dungeon;if(!buildCastleDungeonCutaway(castle,level,dungeon,&error)){std::cerr<<error;return 1;}
    write(dungeon,"dungeon-"+std::to_string(level)+".json");
  }
  std::cout<<"Gothic castle: "<<castle.wings.size()<<" wings, "<<castle.towers.size()<<" towers, "<<castle.dungeonRooms.size()<<" dungeon rooms, "<<model.mesh.triangles.size()<<" triangles\n";
  return 0;
}

int worldHistoryPreview(int argc,char **argv) {
  const std::filesystem::path output=argc>2?argv[2]:"build/phase7/world-history";
  WorldPipelineSettings settings;settings.brief.seed=17;settings.brief.extentMetres=12000;settings.brief.reliefMetres=900;
  settings.brief.tags={Tag::Coast,Tag::Lowland};settings.cellSpacingMetres=400;settings.historyYears=argc>3?uint32_t(std::stoul(argv[3])):200;
  settings.historyTowns=1;settings.historyVillages=2;settings.historyHamlets=2;settings.historyRoadVillages=1;
  GeneratedWorld world;std::string error;if(!generateWorld(settings,world,{},&error)){std::cerr<<error;return 1;}
  const auto &c=*world.civilization;json report={{"seed",17},{"years",settings.historyYears},{"sourcePipeline","native-world-history"},{"settlements",json::array()}};
  std::filesystem::create_directories(output);
  for(const auto &one:c.history.settlements) {
    auto current=worldStorySettlementJson(c,one);
    const auto filename="settlement-"+std::to_string(one.settlement)+".json";std::ofstream(output/filename)<<current.dump();
    report["settlements"].push_back({{"id",one.settlement},{"node",one.node},{"role",settlementClassName(one.role)},
      {"population",one.brief.demand.population},{"foundingPopulation",c.history.foundingPopulations.at(one.node)},{"buildings",one.buildings.size()},{"history",filename}});
    for(const auto &site:c.kingdom.settlements)if(site.id==one.settlement)report["settlements"].back()["worldCell"]=site.cell;
  }
  std::ofstream(output/"story-graph.json")<<worldStoryJson(*c.story).dump();
  report["seconds"]=world.totalSeconds;report["graphFile"]="story-graph.json";
  report["storyEvents"]=c.story->stories.size();
  std::ofstream(output/"world-history.json")<<report.dump(2);std::cout<<report.dump(2)<<'\n';return 0;
}

int exportWorldSurface(int argc,char **argv) {
  if(argc!=5){std::cerr<<"--world-surface OUTPUT SEED RELIEF";return 2;}
  WorldPipelineSettings settings;settings.brief.seed=std::stoull(argv[3]);settings.brief.reliefMetres=std::stod(argv[4]);
  settings.brief.extentMetres=12000;settings.cellSpacingMetres=400;settings.brief.tags={Tag::Coast,Tag::Lowland};
  GeneratedWorld world;std::string error;if(!generateWorld(settings,world,{},&error)){std::cerr<<error;return 1;}
  json j={{"seed",settings.brief.seed},{"extent",12000},{"vertices",json::array()},{"land",json::array()},{"triangles",json::array()}};
  const auto side=world.surface.gridSide();const double spacing=world.surface.spacingMetres();
  for(uint32_t z=0;z<side;++z)for(uint32_t x=0;x<side;++x){
    const double px=x*spacing,pz=z*spacing;double best=1e100;bool land=false;
    for(const auto id:leafNodes(world.graph)){const auto &node=world.graph.node(id);const double d=std::hypot(px-node.site.x,pz-node.site.y);if(d<best){best=d;land=node.land;}}
    j["vertices"].push_back({px,world.surface.at(x,z).elevationMetres,pz});j["land"].push_back(land);
    if(x+1<side&&z+1<side){const auto a=z*side+x;j["triangles"].push_back({a,a+side,a+1});j["triangles"].push_back({a+1,a+side,a+side+1});}
  }
  const std::filesystem::path path=argv[2];std::filesystem::create_directories(path.parent_path());std::ofstream(path)<<j.dump();return 0;
}

int main(int argc, char **argv)
{
  if(argc>=2 && std::string(argv[1])=="--world-surface")return exportWorldSurface(argc,argv);
  if(argc>=2 && std::string(argv[1])=="--world-history")return worldHistoryPreview(argc,argv);
  if(argc>=2 && (std::string(argv[1])=="--architectural-gallery" || std::string(argv[1])=="--religious-gallery")) return planArchitecturalGallery(argc,argv);
  if(argc>=2 && std::string(argv[1])=="--castle") return planOneCastle(argc,argv);
  if(argc>=2 && std::string(argv[1])=="--block") return planOneBlock(argc,argv);
  if(argc>=3 && (std::string(argv[1])=="--street-junction-demo" || std::string(argv[1])=="--street-quarter" || std::string(argv[1])=="--hill-block-demo" || std::string(argv[1])=="--canal-demo" || std::string(argv[1])=="--ground-demo" || std::string(argv[1])=="--ground-only-demo" || std::string(argv[1])=="--dense-street-demo" || std::string(argv[1])=="--terraced-street-demo" || std::string(argv[1])=="--mountain-city" || std::string(argv[1])=="--mountain-base")) {
    const bool streetQuarter=std::string(argv[1])=="--street-quarter";
    const bool hillDemo=streetQuarter || std::string(argv[1])=="--hill-block-demo";
    const bool mountainCity=std::string(argv[1])=="--mountain-city" || std::string(argv[1])=="--mountain-base";
    CityBaseSettings mountainSettings;
    if(!mountainCity && argc!=3){std::cerr<<"This preview accepts one output path\n";return 1;}
    for(int i=3;i<argc;i+=2) {
      if(i+1>=argc){std::cerr<<"Missing mountain-city option value\n";return 1;}
      const std::string option=argv[i];
      if(option=="--seed")mountainSettings.seed=std::stoull(argv[i+1]);
      else if(option=="--tiers")mountainSettings.tiers=std::stoul(argv[i+1]);
      else if(option=="--width")mountainSettings.width=std::stod(argv[i+1]);
      else if(option=="--lane-meander")mountainSettings.laneMeanderMetres=std::stod(argv[i+1]);
      else if(option=="--layout") {
        const std::string v=argv[i+1];
        if(v=="tapered")mountainSettings.layout=CityBaseLayout::Tapered;
        else if(v=="staggered")mountainSettings.layout=CityBaseLayout::Staggered;
        else if(v=="broad")mountainSettings.layout=CityBaseLayout::Broad;
        else if(v=="voronoi")mountainSettings.layout=CityBaseLayout::Voronoi;
        else if(v=="districts")mountainSettings.layout=CityBaseLayout::Districts;
        else {std::cerr<<"layout must be tapered, staggered, broad, voronoi or districts";return 1;}
      }
      else if(option=="--terrain") {
        const std::string v=argv[i+1];
        if(v=="hillside")mountainSettings.terrain=CityBaseTerrain::Hillside;
        else if(v=="ridge")mountainSettings.terrain=CityBaseTerrain::Ridge;
        else if(v=="valley")mountainSettings.terrain=CityBaseTerrain::Valley;
        else {std::cerr<<"terrain must be hillside, ridge or valley";return 1;}
      }
      else {std::cerr<<"Unknown mountain-city option: "<<option;return 1;}
    }
    const bool canalDemo=std::string(argv[1])=="--canal-demo";
    const bool terracedStreet=std::string(argv[1])=="--terraced-street-demo";
    const bool denseStreet=hillDemo || std::string(argv[1])=="--dense-street-demo";
    const bool groundOnly=std::string(argv[1])=="--ground-only-demo" || std::string(argv[1])=="--mountain-base";
    CityCompletionPlan city;city.revision=41;
    const auto rectangle=[](double x,double z,double w,double d){return Poly{{x,0,z},{x+w,0,z},{x+w,0,z+d},{x,0,z+d}};};
    CitySmoothedRoute paved;paved.id=1;paved.halfWidth=2.5;paved.path={{-28,0,0},{28,0,0}};paved.groundCover=CityGroundCover::Stone;
    CitySmoothedRoute dirt;dirt.id=2;dirt.halfWidth=1.8;dirt.path={{0,0,-28},{0,0,28}};dirt.groundCover=CityGroundCover::Dirt;
    city.routes={paved,dirt};
    if(std::string(argv[1])=="--street-junction-demo") {
      paved.halfWidth=4.;paved.path={{-8,0,-32},{-8,0,-8},{-3,0,3},{2,0,12},{2,0,32}};
      dirt.halfWidth=2.5;dirt.groundCover=CityGroundCover::Stone;dirt.path={{-32,0,12},{-3,0,3},{32,0,3}};
      auto feeder=paved;feeder.id=3;feeder.halfWidth=1.2;
      city.routes={paved,dirt,feeder};
    }
    for(uint64_t k=0;k<4;++k) {
      const double x=k%2?0:-28,z=k/2?0:-28;
      const auto use=k==0?DistrictUse::WorkerHousing:k==1?DistrictUse::Craftsmen:k==2?DistrictUse::WealthyResidential:DistrictUse::Merchant;
      city.skeleton.regions.push_back({100+k,100+k,0,CityRegionKind::Block,CityLandUse::UrbanBlock,use,rectangle(x,z,28,28),false,784});
      city.skeleton.regions.push_back({200+k,200+k,100+k,CityRegionKind::Lot,CityLandUse::UrbanLot,use,rectangle(x+5,z+5,18,18),false,324});
      CityLotDevelopment lot;lot.lot=200+k;lot.desiredRooms=k==0?2:4;lot.preferredFootprintMetres2=k==0?32:72;lot.maximumFloors=k==0?1:2;
      lot.residence=k==3?CityResidenceKind::Inn:CityResidenceKind::House;
      lot.groundCover=k>=2?CityGroundCover::Stone:CityGroundCover::Dirt;
      city.lots.push_back(lot);
    }
    if(groundOnly) {
      // Four distinct plots per block: keep ownership even though all ground
      // shares one render mesh and a common world-space blend field.
      city.lots.clear();
      city.skeleton.regions.erase(std::remove_if(city.skeleton.regions.begin(),city.skeleton.regions.end(),[](const auto &r){return r.kind==CityRegionKind::Lot;}),city.skeleton.regions.end());
      for(uint64_t b=0;b<4;++b)for(uint64_t p=0;p<4;++p) {
        const double x=(b%2?0.:-28.)+4.+double(p%2)*12.,z=(b/2?0.:-28.)+4.+double(p/2)*12.;
        const auto id=200+b*4+p;
        city.skeleton.regions.push_back({id,id,100+b,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Craftsmen,rectangle(x,z,9,9),false,81});
        CityLotDevelopment lot;lot.lot=id;lot.groundCover=(b+p)%3==0?CityGroundCover::Grass:(b+p)%3==1?CityGroundCover::Dirt:CityGroundCover::Stone;
        city.lots.push_back(lot);
      }
    }
    if(denseStreet) {
      city.skeleton.regions.clear();city.lots.clear();city.routes.clear();
      CitySmoothedRoute lane;lane.id=1;lane.halfWidth=2.;lane.path={{-4,0,0},{44,0,0}};lane.groundCover=CityGroundCover::Stone;city.routes.push_back(lane);
      for(uint64_t side=0;side<2;++side) {
        const double z=side?2.:-16.;
        city.skeleton.regions.push_back({100+side,100+side,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Craftsmen,rectangle(0,z,44,14),false,616});
        double x=0;
        for(uint64_t i=0;i<8;++i) {
          const double width=4.4+1.1*double(i%3);const auto id=200+side*8+i;
          city.skeleton.regions.push_back({id,id,100+side,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Craftsmen,rectangle(x,z,width,14),false,width*14});
          CityLotDevelopment lot;lot.lot=id;lot.rowHouse=true;lot.merchantHouse=i%2==0;lot.minimumFloors=2+(i+side)%3;lot.maximumFloors=lot.minimumFloors;lot.desiredRooms=3+i%3;lot.preferredFootprintMetres2=width*12;lot.groundCover=CityGroundCover::Stone;city.lots.push_back(lot);x+=width;
        }
      }
      if(!hillDemo)for(uint64_t i=0;i<2;++i) {
        const auto id=300+i;city.skeleton.regions.push_back({id,id,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Suburb,rectangle(4+double(i)*16,20,12,12),false,144});
        CityLotDevelopment lot;lot.lot=id;lot.maximumFloors=1;lot.desiredRooms=1;lot.preferredFootprintMetres2=24;lot.groundCover=CityGroundCover::Dirt;city.lots.push_back(lot);
      }
    }
    CityReliefPlan terracePlan;
    if(terracedStreet) {
      city.skeleton.regions.clear();city.lots.clear();city.routes.clear();city.skeleton.revision=41;
      for(uint64_t row=0;row<3;++row) {
        const double z=row*30.,shift=row==1?-3.:row==2?3.:0.;
        city.skeleton.regions.push_back({100+row,100+row,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Merchant,rectangle(shift-4,z+2,68,20),false,1360});
        for(uint64_t col=0;col<6;++col) {
          const uint64_t id=200+row*6+col;
          city.skeleton.regions.push_back({id,id,100+row,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Merchant,rectangle(shift+col*10.,z+2,10,16),false,160});
          if((row==1 && col==2) || (row==2 && col==4)) {
            city.skeleton.regions.back().kind=CityRegionKind::Reservation;
            city.skeleton.regions.back().lotProgramme="stair-court";
            continue;
          }
          CityLotDevelopment lot;lot.lot=id;lot.rowHouse=true;lot.merchantHouse=col%3==1;lot.minimumFloors=2+(row+col)%2;lot.maximumFloors=lot.minimumFloors;lot.desiredRooms=5;lot.preferredFootprintMetres2=120;lot.groundCover=CityGroundCover::Stone;
          lot.residence=col==2?CityResidenceKind::Inn:col==4?CityResidenceKind::Pub:CityResidenceKind::House;city.lots.push_back(lot);
        }
        city.skeleton.roadNodes.push_back({1+row*2,CityRoadNodeKind::Endpoint,{shift-6,0,z},{}});
        city.skeleton.roadNodes.push_back({2+row*2,CityRoadNodeKind::Endpoint,{shift+66,0,z},{}});
      }
      const auto route=[&](uint64_t id,uint64_t from,uint64_t to,double width) {
        auto a=city.skeleton.roadNodes[from-1].position,b=city.skeleton.roadNodes[to-1].position;
        CityRoadEdge edge;edge.id=id;edge.source=id;edge.from=from;edge.to=to;edge.path={a,b};edge.halfWidth=width;city.skeleton.roadEdges.push_back(edge);
        CitySmoothedRoute r;r.id=id;r.halfWidth=width;r.path={a,b};r.groundCover=CityGroundCover::Stone;city.routes.push_back(r);
      };
      for(uint64_t row=0;row<3;++row)route(1+row,1+row*2,2+row*2,2.);

      CityReliefSettings settings;settings.stairStreets=true;settings.steppedWayGrade=.12;
      std::string why;
      if(!planCityRelief(city.skeleton,[](double,double){return 0.;},settings,terracePlan,&why)){std::cerr<<why;return 1;}
      std::vector<CityReliefPlan::RaisedBlock> decks;
      for(uint64_t row=0;row<3;++row) {
        const double z=row*30.,level=.8+row*4.2,shift=row==1?-3.:row==2?3.:0.;
        const double left=shift-4,right=shift+64,front=z-4,back=z+26;
        CityReliefPlan::RaisedBlock deck;deck.block=100+row;deck.level=level;
        deck.boundary={{left+2,0,front},{right-2,0,front},{right,0,front+2},{right,0,back-2},
          {right-2,0,back},{left+2,0,back},{left,0,back-2},{left,0,front+2}};
        deck.accessFrom={30,0,z-16};deck.accessTo={30,level,front};deck.stairHalfWidth=2.4;
        if(row>0) {
          const double slotLeft=shift+(row==1?20:40),slotRight=slotLeft+10,depth=row==1?10.6:23.;
          deck.boundary={{left+2,0,front},{slotLeft,0,front},{slotLeft,0,front+depth},
            {slotRight,0,front+depth},{slotRight,0,front},{right-2,0,front},{right,0,front+2},
            {right,0,back-2},{right-2,0,back},{left+2,0,back},{left,0,back-2},{left,0,front+2}};
          if(row==1) {
            deck.stairHalfWidth=1.05;
            const double a=slotLeft+1.2,b=slotRight-1.2;
            deck.accessPath={{a,level-4.2,front},{b,level-2.8,front},{b,level-2.8,front+2.6},
              {a,level-1.4,front+2.6},{a,level-1.4,front+5.2},{b,level,front+5.2},{b,level,front+depth}};
          } else {
            deck.accessFrom={slotLeft+5,level-4.2,front};deck.accessTo={slotLeft+5,level,front+depth};
            deck.bridges.push_back({{slotLeft,level,z},{slotRight,level,z},1.3,1.6,.4});
          }
        }
        decks.push_back(deck);
      }
      if(!planRaisedCityBlocks(city.skeleton,decks,settings,terracePlan,&why)){std::cerr<<why;return 1;}
      std::cout<<"Raised blocks: "<<terracePlan.platforms.size()<<", stairs: "<<terracePlan.stairs.size()<<"\n";
    }
    std::vector<Position> baseLandSites;std::vector<Poly> baseLandCells;
    if(mountainCity) {
      CityBasePlan base;std::string why;
      if(!planCityBase(mountainSettings,base,&why)){std::cerr<<why;return 1;}
      baseLandSites=base.landSites;baseLandCells=base.landCells;
      if(groundOnly){city=base.circulation;terracePlan=base.relief;}
      else if(!populateCityBase(base,city,terracePlan,&why)){std::cerr<<why;return 1;}
      std::cout<<"Base-first city: "<<terracePlan.platforms.size()<<" terraces, "<<terracePlan.stairs.size()<<" flights, "
        <<city.routes.size()<<" alleys, "<<terracePlan.rocks.size()<<" rock formations, "<<city.lots.size()<<" lots\n";
    }
    if(canalDemo) {
      city.routes[0].halfWidth=3.2;
      terracePlan.ground=terracePlan.openGround=[](double,double){return 0.;};
      for(int side:{-1,1}){CityWallStructureSegment wall;wall.id=900+side;wall.a={side*25.,0,-28};wall.b={side*25.,0,28};wall.width=1.2;wall.height=5;city.wallSegments.push_back(wall);}
      if(!planCityWaterway(city,terracePlan,1)){std::cerr<<"No safe canal route";return 1;}
      std::cout<<"Waterways: "<<city.waterways.size()<<", crossings: "<<city.waterways[0].bridges.size()<<", wall portals: "<<city.waterways[0].portals.size()<<"\n";
    }
    if(streetQuarter && !planCityStreetQuarter(41,city)){std::cerr<<"street quarter planning failed";return 1;}
    if(hillDemo) {
      terracePlan.ground=terracePlan.openGround=[](double x,double z){return .085*x+.03*z+.12*std::sin(x*.1)*std::sin(z*.2);};
      planCityHillBlocks(city,terracePlan,{});
      if(streetQuarter) {
        CityReliefPlan::RaisedBlock terrace;terrace.block=300;terrace.boundary=rectangle(44,-6,14,16);
        terrace.level=4.15;terrace.terrainCut=true;terrace.stairHalfWidth=1.2;
        terrace.accessFrom={38,terracePlan.openGround(38,0),0};terrace.accessTo={44,terrace.level,0};
        std::string why;
        if(!planRaisedCityBlocks(city.skeleton,{terrace},{},terracePlan,&why)){std::cerr<<why;return 1;}
      }
    }
    CityModelPlan model;SettlementFabric fabric;CityModelSettings options;options.groundVertexSpacing=1.5;std::string error;
    options.groundOnly=groundOnly;
    if(!buildPreviewCityModel(city,fabric,options,model,&error,(terracedStreet||mountainCity||canalDemo||hillDemo)?&terracePlan:nullptr)){std::cerr<<error<<"\n";return 1;}
    json j;j["sourcePipeline"]="native-district";j["previewBounds"]=json::array();for(const auto &deck:terracePlan.platforms){for(auto p:deck.boundary)j["previewBounds"].push_back({p.x,deck.level,p.z});for(auto p:{deck.accessFrom,deck.accessTo})j["previewBounds"].push_back({p.x,p.y,p.z});}j["platforms"]=json::array();for(const auto &deck:terracePlan.platforms)j["platforms"].push_back({{"block",deck.block},{"level",deck.level},{"foundation",deck.foundation},{"boundary",poly(deck.boundary)}});j["stairs"]=json::array();for(const auto &stair:terracePlan.stairs)j["stairs"].push_back({{"edge",stair.edge},{"from",{stair.a.x,stair.a.y,stair.a.z}},{"to",{stair.b.x,stair.b.y,stair.b.z}},{"bands",stair.bands}});j["vertices"]=json::array();for(auto p:model.mesh.vertices)j["vertices"].push_back({p.x,p.y,p.z});
    j["landBlocks"]=json::array();for(const auto &b:terracePlan.landBlocks)j["landBlocks"].push_back({{"block",b.block},{"kind",int(b.kind)},{"boundary",poly(b.boundary)},{"lots",b.lots},{"grade",b.maximumGrade},{"minimum",b.minimum},{"maximum",b.maximum}});
    if(hillDemo)j["generation"]={{"mode","hill-blocks"},{"seed",41},{"landBlocks",terracePlan.landBlocks.size()}};
    if(canalDemo)j["generation"]={{"mode","stone-canal"},{"waterways",city.waterways.size()},{"bridges",city.waterways.front().bridges.size()},{"wallPortals",city.waterways.front().portals.size()}};
    if(mountainCity) {
      j["generation"]={{"mode","base-first-mountain-city"},{"seed",mountainSettings.seed},{"tiers",terracePlan.platforms.size()},{"baseOnly",groundOnly}};
      j["generation"]["layout"]=mountainSettings.layout==CityBaseLayout::Districts?"districts":mountainSettings.layout==CityBaseLayout::Voronoi?"voronoi":mountainSettings.layout==CityBaseLayout::Broad?"broad":mountainSettings.layout==CityBaseLayout::Staggered?"staggered":"tapered";
      j["landSites"]=poly(baseLandSites);j["landCells"]=json::array();for(const auto &cell:baseLandCells)j["landCells"].push_back(poly(cell));
      j["alleys"]=json::array();for(const auto &route:city.routes)j["alleys"].push_back({{"id",route.id},{"path",poly(route.path)},{"width",route.halfWidth*2}});
      j["rocks"]=json::array();for(const auto &rock:terracePlan.rocks)j["rocks"].push_back({{"centre",{rock.centre.x,rock.centre.y,rock.centre.z}},{"radius",rock.radius},{"height",rock.height}});
      j["lots"]=json::array();for(const auto &region:city.skeleton.regions)if(region.kind==CityRegionKind::Lot)
        j["lots"].push_back({{"id",region.id},{"block",region.parent},{"boundary",poly(region.boundary)},{"level",terracePlan.regionLevel.at(region.id)}});
    }
    j["triangles"]=model.mesh.triangles;j["materials"]=model.materials;j["triangleUVs"]=model.triangleUVs;
    j["surfaceMaterials"]=model.surfaceMaterials;j["groundWeights"]=model.groundWeights;j["groundMaterialIds"]=model.groundMaterialIds;
    j["textureManifest"]="assets/procedural/textures/medieval-pixel/materials.json";j["buildings"]=json::array();
    if(groundOnly) {
      j["vertexTextureWeights"]=json::array();
      for(auto p:model.mesh.vertices)j["vertexTextureWeights"].push_back(cityGroundWeights(model.surfaces,p.x,p.z));
      j["groundOnly"]=true;
    }
    for(const auto &b:model.buildings){json footprint=json::array();for(auto p:b.footprint)footprint.push_back({p.x,p.y,p.z});
      j["buildings"].push_back({{"id",b.id},{"kind",cityModelBuildingKindName(b.kind)},{"footprint",footprint},{"base",b.base},{"wallHeight",b.wallHeight},{"roofHeight",b.roofHeight},{"rooms",b.rooms},{"windows",b.windows},{"roomConnections",b.roomConnections},{"exteriorDoors",poly(b.exteriorDoors)},{"jettyMetres",b.jettyMetres},{"interiorViews",poly(b.interiorViews)},{"trade",b.trade}});
      j["buildings"].back()["roomList"]=json::array();for(const auto &room:b.roomList)j["buildings"].back()["roomList"].push_back(roomJson(room));}
    const std::filesystem::path path=argv[2];if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
    writeCityInstances(path,model,j);std::ofstream(path)<<j.dump();std::cout<<model.buildings.size()<<" buildings, "<<model.mesh.triangles.size()<<" triangles\n";
    if(streetQuarter)return model.buildings.size()==12?0:1;
    if(canalDemo)return model.buildings.empty()?1:0;
    if(mountainCity)return groundOnly ? (model.buildings.empty()?0:1) : (model.buildings.empty()?1:0);
    return model.buildings.size()==(groundOnly?0:terracedStreet?16:hillDemo?16:denseStreet?18:4)?0:1;
  }
  WorldPipelineSettings settings;
  settings.brief.extentMetres = 12000;
  settings.brief.seed = 17;
  settings.brief.reliefMetres = 900;
  settings.brief.tags = {Tag::Coast, Tag::Lowland};
  settings.cellSpacingMetres = 400;
  settings.hierarchyLevels = 1;
  WorldSiteSelectionSettings siteSettings;
  uint32_t population = 120;
  uint32_t kingdomCount = 0; // --kingdom N: compose the first N kingdom settlements for the world sheet.
  uint32_t kingdomHistoryYears = 200; // --kingdom-history N: years of one kingdom-wide history the settlements are composed from; 0 is synthetic.
  uint64_t variant = 0;      // --variant K: jitter the fabric grammar (0 = the class recipe).
  int recipe = -1;           // --recipe organic|planned|lsystem: force the layout recipe.
  int pattern = -1;          // --pattern basic|raster|radial|elevation: road growth pattern.
  bool cityRegionOnly = false; // Build the selected world city, not kingdom-wide road meshes.
  bool lotsOnly = false;     // --lots 1: render the Watabou lot allocation without houses.
  bool roads = false;        // --roads 1: the roads-first settlement generator (no wall).
  // --city wfc: the irregular quad grid path. --cell-size and --hex-radius size it.
  bool cityWfc = false;
  procedural::world::citywfc::CityGridSettings cityWfcSettings;
  // --hills <metres>: the town stands on a hill of that amplitude instead of
  // being levelled onto one plane. --hill-wavelength sets how long the hill is
  // and --street-grade the steepest street before a way becomes steps.
  bool releaseCheck = false;
  json historicalProvenance;
  bool raisedDistricts = false;
  double hills = 0, hillWavelength = 190, streetGrade = .12, terraceRise=0, terraceCutFill=2.5;
  CityModelSettings cityMeshSettings;
  cityMeshSettings.instanceDwellingPanels=std::getenv("CITY_FLAT_MESH")==nullptr;
  bool voxelAssemblyAudit=false, voxelAuditOnly=false, voxelNativeSample=false;
  bool voxelNativeExport=false;
  bool cityGroundOverlay=false;
  std::string worldMapExport;
  uint32_t worldMapExportResolution=1024;
  virtualgeometry::CityVoxelDwellingBlockSettings voxelBlocks;
  std::string voxelBlockSweep;
  std::string voxelBlockDump;
  // Far proxy voxel = this many 10 cm cells. At 4 a house decoded to ~3,000
  // triangles and 6,000 vertices, more than all near blocks around it.
  int32_t voxelFarFactor=8;
  bool voxelTracePages=false; // Hardware ray-trace pages; raster-only by default.
  uint32_t voxelNativeSampleLimit=10;
  std::filesystem::path output = "settlement-preview";
  for (int i = 1; i + 1 < argc; i += 2)
  {
    std::string option = argv[i], value = argv[i + 1];
    if (option == "--seed")
    {
      settings.brief.seed = std::stoull(value);
    }
    else if (option == "--history-limit") settings.historyMaximumLiving=uint32_t(std::stoul(value));
    else if (option == "--history-towns") settings.historyTowns=uint32_t(std::stoul(value));
    else if (option == "--history-villages") settings.historyVillages=uint32_t(std::stoul(value));
    else if (option == "--history-hamlets") settings.historyHamlets=uint32_t(std::stoul(value));
    else if (option == "--history-road-villages") settings.historyRoadVillages=uint32_t(std::stoul(value));
    else if (option == "--release-check") releaseCheck=std::stoi(value)!=0;
    else if (option == "--city-region") cityRegionOnly=std::stoi(value)!=0;
    else if (option == "--city") cityWfc = value == "wfc";
    else if (option == "--cell-size") cityWfcSettings.lattice.targetCellEdgeMetres = std::stod(value);
    else if (option == "--hex-radius") cityWfcSettings.lattice.hexRadiusTriangles = uint32_t(std::stoul(value));
    else if (option == "--city-infrastructure")
    {
      cityMeshSettings.infrastructureOnly=std::stoi(value)!=0;
      cityMeshSettings.groundVertexSpacing=6.;
    }
    else if (option == "--voxel-assembly-audit") voxelAssemblyAudit=std::stoi(value)!=0;
    else if (option == "--voxel-native-sample") {
      voxelNativeSample=std::stoi(value)!=0;
      voxelAssemblyAudit|=voxelNativeSample;
    }
    else if (option == "--voxel-native-sample-limit") voxelNativeSampleLimit=uint32_t(std::stoul(value));
    else if (option == "--voxel-block-across") voxelBlocks.horizontalVoxels=uint32_t(std::stoul(value));
    else if (option == "--voxel-block-up") voxelBlocks.verticalVoxels=uint32_t(std::stoul(value));
    else if (option == "--voxel-block-sweep") voxelBlockSweep=value; // e.g. 22x22,22x30,44x30
    else if (option == "--voxel-block-dump") voxelBlockDump=value;
    else if (option == "--voxel-far-factor") voxelFarFactor=std::max(1,std::stoi(value));
    else if (option == "--voxel-trace-pages") voxelTracePages=std::stoi(value)!=0;
    else if (option == "--voxel-native-export") {
      voxelNativeExport=std::stoi(value)!=0;
      voxelNativeSample|=voxelNativeExport;
      voxelAssemblyAudit|=voxelNativeExport;
    }
    else if (option == "--world-map-export") worldMapExport=value;
    else if (option == "--world-map-export-resolution") worldMapExportResolution=uint32_t(std::stoul(value));
    else if (option == "--city-ground-overlay") {
      cityGroundOverlay=std::stoi(value)!=0;
      voxelAssemblyAudit|=cityGroundOverlay;
    }
    else if (option == "--voxel-audit-only") {
      voxelAuditOnly=std::stoi(value)!=0;
      voxelAssemblyAudit|=voxelAuditOnly;
    }
    else if (option == "--terrace-cut-fill")terraceCutFill=std::stod(value);
    else if (option == "--ground-spacing")cityMeshSettings.groundVertexSpacing=std::stod(value);
    else if (option == "--street-carve")cityMeshSettings.streetCarveMetres=std::stod(value);
    else if (option == "--fences")cityMeshSettings.lotBoundaries=std::stoi(value)!=0;
    else if (option == "--warp")cityMeshSettings.warpToLot=std::stoi(value)!=0;
    else if (option == "--street-pavement")cityMeshSettings.streetPavementMetres=std::stod(value);
    else if (option == "--extent")
    {
      settings.brief.extentMetres = std::stod(value) * 1000;
    }
    else if (option == "--spacing")
    {
      settings.cellSpacingMetres = std::stod(value) * 1000;
    }
    else if (option == "--relief")
    {
      settings.brief.reliefMetres = std::stod(value);
    }
    else if (option == "--levels")
    {
      settings.hierarchyLevels = uint32_t(std::stoul(value));
    }
    else if (option == "--history-snapshot")
    {
      std::ifstream file(value);json history;
      if(!file || !(file>>history) || !history.contains("householdRecords")) {
        std::cerr<<"history snapshot requires householdRecords\n";return 1;
      }
      historicalProvenance={{"snapshot",value},{"year",history.value("year",0u)},
        {"week",history.value("week",0u)},{"settlement",history.value("settlement",uint64_t(0))},
        {"socialSource",history.value("socialSource",std::string("unspecified"))},
        {"defenceCircuits",history.value("defenceCircuits",json(nullptr))}};
      if (history.contains("defenceCircuits")) {
        siteSettings.historicalEnclosures.emplace();
        for (const auto &entry : history.at("defenceCircuits")) {
          SettlementHistoricalEnclosure enclosure;
          enclosure.id=entry.at("id").get<uint64_t>();
          enclosure.era=entry.at("era").get<uint64_t>();
          enclosure.closed=entry.value("closed",false);
          enclosure.absorbed=entry.value("lifecycle",std::string{})=="absorbed";
          enclosure.abandoned=entry.value("abandoned",false);
          enclosure.palisade=entry.value("palisade",false);
          if (!entry.at("closedWeek").is_null()) enclosure.closedWeek=entry.at("closedWeek").get<uint32_t>();
          if (enclosure.closedWeek>history.at("week").get<uint32_t>()) enclosure.closed=false;
          enclosure.buildings=entry.at("enclosedBuildings").get<std::vector<uint64_t>>();
          siteSettings.historicalEnclosures->push_back(std::move(enclosure));
        }
      }
      siteSettings.historicalHouseholds.emplace();
      for(const auto &h:history.at("householdRecords"))
        siteSettings.historicalHouseholds->push_back({h.at("id").get<uint64_t>(),h.at("members").get<uint32_t>(),h.at("workers").get<uint32_t>(),h.value("collective",false)});
      CityPopulationDemand historical;std::string why;
      if(!deriveCityPopulationDemand(*siteSettings.historicalHouseholds,{},historical,&why)) {
        std::cerr<<why<<'\n';return 1;
      }
      population=historical.population;
      siteSettings.historicalBuildings.emplace();
      siteSettings.historicalManor=false;
      siteSettings.historicalCastle=false;
      for(const auto &building:history.at("buildings")) {
        const auto use=building.at("use").get<std::string>();
        siteSettings.historicalBuildings->push_back({building.at("id").get<uint64_t>(),use,
          building.at("builtWeek").get<uint32_t>(),building.at("floors").get<uint32_t>(),building.value("residents",0u),
          building.value("trade",std::string{}), (history.at("week").get<uint32_t>()-building.at("builtWeek").get<uint32_t>())/52});
        if(use=="manor")siteSettings.historicalManor=true;
        if(use=="keep")siteSettings.historicalCastle=true;
      }
    }
    else if (option == "--population")
    {
      population = uint32_t(std::stoul(value));
    }
    else if (option == "--cell")
    {
      siteSettings.preferredCell = uint32_t(std::stoul(value));
    }
    else if (option == "--output")
    {
      output = value;
    }
    else if (option == "--kingdom")
    {
      kingdomCount = uint32_t(std::stoul(value));
    }
    else if (option == "--kingdom-history")
    {
      kingdomHistoryYears = uint32_t(std::stoul(value));
    }
    else if (option == "--village-form")
    {
      using Form = WorldSiteSelectionSettings::VillageForm;
      siteSettings.villageForm = value == "cluster"     ? Form::Cluster
                                 : value == "street"    ? Form::Street
                                 : value == "round"     ? Form::Round
                                 : value == "anger"     ? Form::Anger
                                 : value == "scattered" ? Form::Scattered
                                                        : Form::Auto;
    }
    else if (option == "--lots")
    {
      lotsOnly = value == "1" || value == "true" || value == "yes";
    }
    else if (option == "--roads")
    {
      roads = value == "1" || value == "true" || value == "yes";
    }
    else if (option == "--terrain-crown") siteSettings.landforms.facetCrownFraction=std::stod(value);
    else if (option == "--landforms") siteSettings.landforms.reliefMetres=std::stod(value);
    else if (option == "--landform-shelves") siteSettings.landforms.shelves=unsigned(std::stoul(value));
    else if (option == "--raised-districts") raisedDistricts=std::stoi(value)!=0;
    else if (option == "--terrace-rise") terraceRise=std::stod(value);
    else if (option == "--hills")
    {
      hills = std::stod(value);
    }
    else if (option == "--hill-wavelength")
    {
      hillWavelength = std::stod(value);
    }
    else if (option == "--street-grade")
    {
      streetGrade = std::stod(value);
    }
    else if (option == "--pattern")
    {
      pattern = value == "raster" ? 1 : value == "radial" ? 2 : value == "elevation" ? 3 : value == "basic" ? 0 : -1;
    }
    else if (option == "--recipe")
    {
      recipe = value == "lsystem" ? 3 : value == "planned" ? 2 : value == "organic" ? 0 : -1;
    }
    else if (option == "--variant")
    {
      // Variant K: jittered class grammar plus a plaza of 6/4/8 sides.
      variant = std::stoull(value);
      siteSettings.plazaSides = variant ? std::array<uint32_t, 3>{6, 4, 8}[variant % 3] : 6;
    }
    else if (option == "--tags")
    {
      settings.brief.tags.clear();
      std::stringstream stream(value);
      std::string name;
      while (std::getline(stream, name, ','))
      {
        for (uint8_t t = 0; t < uint8_t(Tag::Count); ++t)
        {
          if (name == tagName(Tag(t)))
          {
            settings.brief.tags.push_back(Tag(t));
          }
        }
      }
    }
    else
    {
      std::cerr << "unknown option " << option << "\n";
      return 1;
    }
  }
  if(siteSettings.historicalHouseholds) {
    CityPopulationDemand historical;std::string why;
    if(!deriveCityPopulationDemand(*siteSettings.historicalHouseholds,{},historical,&why)){std::cerr<<why<<'\n';return 1;}
    population=historical.population; // --population cannot override an authoritative snapshot.
  }
  siteSettings.population = {settings.brief.seed, population, population};
  std::filesystem::create_directories(output);
  std::string error;
  GeneratedWorld world;
  const auto stageClock=std::chrono::steady_clock::now();
  auto stageMark=stageClock;
  const auto stage=[&](const char *what) {
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-stageMark).count();
    stageMark=std::chrono::steady_clock::now();
    std::fprintf(stderr,"stage %s: %.0f s (%.0f s total)\n",what,seconds,
      std::chrono::duration<double>(stageMark-stageClock).count());
  };

  settings.historyYears = kingdomHistoryYears;
  if (!generateWorldDirected(settings, world, {}, &error))
  {
    std::cerr << "world generation failed: " << error << "\n";
    return 1;
  }
  stage("world generation");
  if (!worldMapExport.empty()) {
    // The renderer's terrain source for this exact world, so a city ground
    // overlay written by the same run lands on the terrain it was graded on.
    MapExportSettings exportSettings{};
    exportSettings.resolution = worldMapExportResolution;
    MapExportBundle bundle;
    if (!buildMapExportBundle(world.graph, world.index, world.surface, world.climate,
            world.rivers, world.coastline, world.contours, world.regions,
            BlendSettings{}, HeightBlendSettings{}, exportSettings, bundle, &error) ||
        !writeMapExportBundle(bundle, worldMapExport, &error)) {
      std::cerr << "world map export failed: " << error << "\n";
      return 1;
    }
    std::fprintf(stderr, "world map export: %s (%.0f m extent)\n", worldMapExport.c_str(),
                 bundle.extentMetres);
    stage("world map export");
  }
  KingdomPlan kingdom;
  std::string kingdomError;
  bool kingdomOK = bool(world.civilization);
  if (kingdomOK) kingdom = world.civilization->kingdom;
  else kingdomOK = selectKingdomSites(world, siteSettings, {}, kingdom, &kingdomError);
  std::cout << "kingdom " << (kingdomOK ? "planned" : "rejected: " + kingdomError) << " settlements=" << kingdom.settlements.size() << " links=" << kingdom.links.size() << "\n";
  // World roads: the Parish-Muller growth over the whole map, seeded by the kingdom links.
  generator::RoadNetwork worldRoads;
  if (kingdomOK && !cityRegionOnly)
  {
    generator::Ledger roadLedger;
    std::string roadError;
    if (generator::WorldRoads::grow(world, kingdom, {}, worldRoads, roadLedger, &roadError))
    {
      for (const auto &entry : roadLedger.entries())
      {
        std::cout << "  " << entry.message << "\n";
      }
      std::cout << "world roads segments=" << worldRoads.segments.size() << " modules=" << worldRoads.modules << " pruned=" << worldRoads.pruned << " rotated=" << worldRoads.rotated << " cut=" << worldRoads.cut
                << " snapped=" << worldRoads.snappedCrossing + worldRoads.snappedRoad << " illegal=" << worldRoads.failedIllegal << "\n";
    }
    else
    {
      std::cout << "world roads rejected: " << roadError << "\n";
    }
  }
  for (auto &r : kingdom.rejections)
  {
    std::cout << "  kingdom: " << r << "\n";
  }
  // The kingdom first, and its history: every site of the plan founded in
  // the legendary age and advanced in one story graph, so the city is composed from
  // the dated brief of its own settlement and not from a synthetic count.
  KingdomHistoryResult kingdomHistory;

  std::map<uint64_t, size_t> kingdomHistoryIndex;
  if (kingdomOK && kingdomHistoryYears > 0)
  {
    kingdomHistory = world.civilization->history;

    std::cout << "kingdom history: " << kingdomHistoryYears << " years in " << kingdomHistory.report.totalSeconds << " s, living " << kingdomHistory.report.living
              << ", households " << kingdomHistory.report.households << ", buildings " << kingdomHistory.report.buildings << ", arrivals " << kingdomHistory.arrivals
              << ", departures " << kingdomHistory.departures << "\n";
    std::filesystem::create_directories(output / "history");
    for (size_t k = 0; k < kingdomHistory.settlements.size(); ++k)
    {
      const auto &one = kingdomHistory.settlements[k];
      kingdomHistoryIndex[one.settlement] = k;
      auto j=worldStorySettlementJson(*world.civilization,one);
      std::ofstream(output / "history" / ("settlement-" + std::to_string(one.settlement) + ".json")) << j.dump();
      std::cout << "  " << settlementClassName(one.role) << " " << one.settlement << ": " << one.brief.demand.population << " people, " << one.brief.households.size() << " households, "
                << one.brief.buildings.size() << " buildings, " << one.enclosures.size() << " enclosures\n";
    }
    std::ofstream(output/"history"/"story-graph.json")<<worldStoryJson(*world.civilization->story).dump();
    stage("kingdom history");
  }
  // The focal city takes its own settlement's records: the one at the
  // requested cell, else the capital. A snapshot given on the command line
  // stays authoritative.
  const auto applyHistory = [&](const KingdomHistorySettlement &one, WorldSiteSelectionSettings &into) {
    into.historicalHouseholds = one.households;
    into.historicalBuildings = one.buildings;
    into.historicalEnclosures = one.enclosures;
    into.historicalManor = one.manor;
    into.historicalCastle = one.castle;
    into.population = {settings.brief.seed, one.brief.demand.population, one.brief.demand.population};
  };
  if (!kingdomHistory.settlements.empty() && !siteSettings.historicalHouseholds)
  {
    size_t focal = 0;
    for (size_t k = 0; k < kingdom.settlements.size(); ++k)
    {
      if (kingdom.settlements[k].cell == siteSettings.preferredCell || (siteSettings.preferredCell == kInvalidNode && kingdom.settlements[k].role == SettlementClass::Capital))
      {
        focal = k;
        break;
      }
    }
    if (focal < kingdomHistory.settlements.size())
    {
      applyHistory(kingdomHistory.settlements[focal], siteSettings);
      siteSettings.preferredCell = kingdom.settlements[focal].cell;
      historicalProvenance = {{"graph", "history/story-graph.json"}, {"year", kingdomHistoryYears}, {"settlement", kingdomHistory.settlements[focal].node},
                              {"worldSettlement",kingdom.settlements[focal].id},{"week",kingdomHistory.settlements[focal].brief.week},
                              {"socialSource", "annual-world-story-graph"},
                              // The story graph dates no circuits: the curtain is the fabric's plan.
                              {"defenceCircuits", json::array()},{"defenceSource", "fabric-plan"}};
      std::cout << "focal city is kingdom " << settlementClassName(kingdom.settlements[focal].role) << " " << kingdom.settlements[focal].id << " with "
                << kingdomHistory.settlements[focal].brief.demand.population << " people from its history\n";
    }
  }
  WorldSettlementSite site;
  bool selected = selectWorldSettlementSite(world, siteSettings, site, &error);
  std::cout << "site " << (selected ? "selected" : "rejected") << " " << error
            << (selected ? std::string(" class=") + settlementClassName(site.settlementClass) + " plaza=" + std::to_string(site.plazaCell) + " temple=" + std::to_string(site.templeCell) : "") << "\n";
  if(selected && !site.landforms.cells.empty()) {
    json diagnostic={{"summit",site.landforms.summit},{"entrance",site.landforms.entrance},{"focus",{site.localCentre.x,site.localCentre.y,site.localCentre.z}}};
    diagnostic["cells"]=json::array();
    for(const auto &c:site.brief.input.cells)diagnostic["cells"].push_back({{"id",c.id},{"boundary",poly(c.boundary)},{"slope",c.slope},{"defense",c.defense},{"buildable",c.buildable},{"allocatable",c.allocatable}});
    diagnostic["shelves"]=json::array();
    for(const auto &c:site.landforms.cells)diagnostic["shelves"].push_back({{"id",c.id},{"boundary",poly(c.boundary)},{"shelf",c.shelf},{"uplift",c.uplift},{"uphill",c.uphill}});
    std::ofstream(output/"landform-plan.json")<<diagnostic.dump(2);
  }
  json report;
  report["world"] = {
    {"seed", settings.brief.seed},
    {"extentMetres", settings.brief.extentMetres},
    {"cellSpacingMetres", settings.cellSpacingMetres},
    {"reliefMetres", settings.brief.reliefMetres},
    {"leafCells", world.leafCells},
    {"landCells", world.landCells}};
  report["siteSelected"] = selected;
  report["kingdom"] = json::array();
  for (auto &settled : kingdom.settlements)
  {
    report["kingdom"].push_back(
        {{"id", settled.id},
         {"cell", settled.cell},
         {"role", settlementClassName(settled.role)},
         {"centre", poly({settled.centre})},
         {"population", settled.population},
         {"parentTown", settled.parentTown},
         {"region", settled.regionName}});
  }
  report["kingdomLinks"] = json::array();
  for (auto &link : kingdom.links)
  {
    report["kingdomLinks"].push_back({{"from", link.from}, {"to", link.to}, {"trunk", link.trunk}});
  }
  report["kingdomRejections"] = kingdom.rejections;
  report["siteError"] = error;
  const double extent = settings.brief.extentMetres;

  // Complete world: compose every kingdom settlement (up to --kingdom N) and
  // keep their street and house footprints in world coordinates.
  struct WorldSettlementSketch
  {
    KingdomSettlement settlement;
    std::vector<Poly> houses, districts, plaza;
    std::vector<std::vector<Position>> streets, walls;
    std::vector<Poly> fields;
    bool composed = false;
    std::string note;
  };

  std::vector<WorldSettlementSketch> sketches;
  json worldJson = json::array();
  for (size_t k = 0; kingdomOK && k < kingdom.settlements.size() && k < kingdomCount; ++k)
  {
    const auto &settled = kingdom.settlements[k];
    WorldSettlementSketch sketch;
    sketch.settlement = settled;
    WorldSiteSelectionSettings each = siteSettings;
    each.historicalHouseholds.reset(); // Other kingdom sites need their own dated snapshots.
    each.historicalManor.reset();each.historicalCastle.reset();each.historicalBuildings.reset();each.historicalEnclosures.reset();
    each.preferredCell = settled.cell;
    each.population = {settings.brief.seed + settled.id, settled.population, settled.population};
    if (const auto own = kingdomHistoryIndex.find(settled.id); own != kingdomHistoryIndex.end())
    {
      applyHistory(kingdomHistory.settlements[own->second], each);
    }
    each.producer = settled.id;
    WorldSettlementSite one;
    std::string oneError;
    if (!selectWorldSettlementSite(world, each, one, &oneError))
    {
      sketch.note = "site: " + oneError;
      sketches.push_back(sketch);
      std::cout << "world " << settlementClassName(settled.role) << " " << settled.id << " rejected: " << oneError << "\n";
      continue;
    }
    CityTerritory oneTerritory;
    if (!generateCityTerritory(one.brief.input, one.brief.suitability, one.brief.settings, one.brief.terrain, oneTerritory, &oneError))
    {
      sketch.note = "territory: " + oneError;
      sketches.push_back(sketch);
      std::cout << "world " << settlementClassName(settled.role) << " " << settled.id << " territory rejected: " << oneError << "\n";
      continue;
    }
    SettlementFabric oneFabric;
    SettlementFabricSettings oneSettings;
    if (settled.alongRoad)
    {
      oneSettings.villageForm = SettlementFabricSettings::VillageForm::Linear;
    }
    bool oneOK = composeSettlementFabric(one, oneTerritory, oneSettings, oneFabric, &oneError);
    auto toWorld = [&](Position p) {
      return Position{p.x + one.origin.x, 0, p.z + one.origin.z};
    };
    for (size_t i = 0; i < oneTerritory.fields.size(); ++i)
    {
      for (const auto &f : oneTerritory.fields[i].fields)
      {
        Poly w;
        for (auto v : f.boundary)
        {
          w.push_back(toWorld(v));
        }
        sketch.fields.push_back(w);
      }
    }
    if (oneOK && oneFabric.layoutAccepted)
    {
      sketch.composed = true;
      for (const auto &o : oneFabric.layout.objects)
      {
        if (o.kind == CityObjectKind::House || o.kind == CityObjectKind::Workshop)
        {
          for (const auto &part : o.parts)
          {
            Poly w;
            for (auto v : part.footprint)
            {
              w.push_back(toWorld(v));
            }
            sketch.houses.push_back(w);
          }
        }
      }
      for (const auto &st : oneFabric.layout.streets)
      {
        std::vector<Position> w;
        for (auto v : st.path)
        {
          w.push_back(toWorld(v));
        }
        sketch.streets.push_back(w);
      }
      for (const auto &wall : oneFabric.layout.walls)
      {
        sketch.walls.push_back({toWorld(wall.a), toWorld(wall.b)});
      }
      for (const auto &d : oneFabric.layout.districts)
      {
        for (const auto &piece : d.pieces)
        {
          Poly w;
          for (auto v : piece)
          {
            w.push_back(toWorld(v));
          }
          (d.use == DistrictUse::Civic ? sketch.plaza : sketch.districts).push_back(w);
        }
      }
    }
    else
    {
      sketch.note = "fabric: " + (oneOK ? std::string("layout rejected") : oneError);
      for (const auto &reason : oneFabric.rejections)
      {
        if (reason.find("layout") != std::string::npos || reason.find("terrace") != std::string::npos || reason.find("gate") != std::string::npos)
        {
          sketch.note += "; " + reason;
        }
      }
    }
    std::cout << "world " << settlementClassName(settled.role) << " " << settled.id << " " << settled.regionName << ": "
              << (sketch.composed ? "streets " + std::to_string(sketch.streets.size()) + " houses " + std::to_string(sketch.houses.size()) : sketch.note) << " fields " << sketch.fields.size() << "\n";
    worldJson.push_back(
        {{"id", settled.id},
         {"role", settlementClassName(settled.role)},
         {"composed", sketch.composed},
         {"streets", sketch.streets.size()},
         {"houses", sketch.houses.size()},
         {"fields", sketch.fields.size()},
         {"note", sketch.note}});
    sketches.push_back(std::move(sketch));
  }
  report["worldSettlements"] = worldJson;
  // Overview: every leaf by biome, rivers, all candidates, the chosen domain.
  {
    const double size = 1400;
    Svg svg(size, size, size / extent);
    for (const auto &n : world.graph.nodes())
    {
      if (n.refined())
      {
        continue;
      }
      Poly p;
      for (auto q : n.area)
      {
        p.push_back({q.x, 0, q.y});
      }
      svg.polygon(p, biomeColour(biomeOf(n)), "#ffffff", .3);
    }
    for (const auto &s : world.rivers.segments)
    {
      svg.polyline({{s.from.x, 0, s.from.y}, {s.to.x, 0, s.to.y}}, "#3d7fd0", .6 + std::sqrt(std::max(0., s.dischargeCumecs)) * .15);
    }
    for (const auto &c : site.candidates)
    {
      svg.circle(c.centre, c.accepted ? 3.5 : 2.5, c.accepted ? "#2ecc40" : "#e0342a", "#222", .4, .85);
    }
    for (const auto &sketch : sketches)
    {
      for (const auto &f : sketch.fields)
      {
        svg.polygon(f, "#d9b85c", "#7a5f10", .3, .9);
      }
      for (const auto &d : sketch.districts)
      {
        svg.polygon(d, "#e0b090", "none", 0, .6);
      }
      for (const auto &p : sketch.plaza)
      {
        svg.polygon(p, "#f0e0a0", "none", 0, .9);
      }
      for (const auto &st : sketch.streets)
      {
        svg.polyline(st, "#f4ecd8", .6);
      }
      for (const auto &h : sketch.houses)
      {
        svg.polygon(h, "#6a3a1a", "none", 0, 1);
      }
      for (const auto &w : sketch.walls)
      {
        svg.polyline(w, "#222", 1.2);
      }
    }
    for (const auto &link : kingdom.links)
    {
      svg.polyline({link.a, link.b}, "#b09a7a", .8, "stroke-dasharray=\"5 4\"");
    }
    for (const auto &sg : worldRoads.segments)
    {
      svg.polyline({sg.a, sg.b}, sg.highway ? "#5a3a1a" : "#8a6a3a", sg.highway ? 2.4 : 1.3);
    }
    for (const auto &settled : kingdom.settlements)
    {
      double radius = settled.role == SettlementClass::Capital ? 11 : settled.role == SettlementClass::Town ? 8 : settled.role == SettlementClass::Village ? 5.5 : 4;
      std::string fill = settled.role == SettlementClass::Capital ? "#ffd400" : settled.role == SettlementClass::Town ? "#ff8c00" : settled.role == SettlementClass::Village ? "#f5f5f5" : "#c8c8c8";
      svg.circle(settled.centre, radius, fill, "#000", 1.4);
      if (settled.role == SettlementClass::Capital || settled.role == SettlementClass::Town)
      {
        svg.text({settled.centre.x, 0, settled.centre.z + 180}, std::string(settlementClassName(settled.role)) + " " + settled.regionName, 14, "#000");
      }
    }
    if (selected)
    {
      svg.polygon(
          {site.origin, {site.origin.x + site.bounds.maxX, 0, site.origin.z}, {site.origin.x + site.bounds.maxX, 0, site.origin.z + site.bounds.maxZ}, {site.origin.x, 0, site.origin.z + site.bounds.maxZ}},
          "none",
          "#ff9900",
          2.5);
      svg.circle(site.centre, 8, "#ffd400", "#000", 1.5);
      svg.text({site.centre.x, 0, site.centre.z + 220}, site.regionName, 16, "#000");
    }
    svg.text({150, 0, 150}, "seed " + std::to_string(settings.brief.seed) + "  " + std::to_string(int(extent / 1000)) + " km  candidates " + std::to_string(site.candidates.size()), 15, "#000", "start");
    std::ofstream(output / "overview.svg") << svg.finish();
  }
  if (!selected)
  {
    std::ofstream(output / "settlement.json") << report.dump(2);
    return 2;
  }
  // --city wfc: the irregular quad grid path, over the planning cells the site
  // selector granted. First milestone - the grid only, nothing labelled on it.
  if (cityWfc)
  {
    using namespace procedural::world::citywfc;
    namespace geom2 = procedural::geom2;

    cityWfcSettings.lattice.seed = settings.brief.seed;
    cityWfcSettings.lattice.site = site.id != 0 ? site.id : uint64_t(site.cell);

    const auto started = std::chrono::steady_clock::now();

    CityGrid grid;
    std::string gridError;
    const bool gridOK = buildCityGrid(site.urbanGround, cityWfcSettings, grid, &gridError);

    const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    if (!gridOK)
    {
      std::cout << "city grid rejected: " << gridError << "\n";
      std::ofstream(output / "settlement.json") << report.dump(2);

      return 3;
    }

    const CityGridQuality &q = grid.quality;

    std::cout << "CITY_WFC_GRID seed=" << settings.brief.seed
              << " site=" << site.regionName
              << " class=" << settlementClassName(site.settlementClass)
              << " cells=" << site.urbanGround.size()
              << " hexes=" << grid.lattice.hexes
              << " faces=" << q.faces
              << " verts=" << q.vertices
              << " edge=" << q.p05Edge << "/" << q.medianEdge << "/" << q.p95Edge
              << " area=" << q.p05Area << "/" << q.medianArea << "/" << q.p95Area
              << " angle=" << q.p05InteriorAngle << "/" << q.minInteriorAngle
              << " aspect=" << q.p95Aspect
              << " coverage=" << q.coverageRatio
              << " deviation=" << q.boundaryDeviationMetres
              << " residual=" << q.residualRatio
              << " peeled=" << q.mergedRim
              << " ms=" << milliseconds << "\n";

    for (const std::string &entry : grid.ledger)
    {
      std::cout << "  grid: " << entry << "\n";
    }

    // The sheet is drawn in the site's local frame, the same frame the planning
    // cells arrived in, so it lines up with every other local sheet.
    const geom2::Box box = geom2::bounds(grid.region);
    const double spanX = geom2::toMetres(box.maxX - box.minX), spanZ = geom2::toMetres(box.maxY - box.minY);
    const double pixelsPerMetre = std::min(1600.0 / std::max(spanX, 1.0), 1600.0 / std::max(spanZ, 1.0));

    static const char *patchPalette[] = {"#4c6ef5", "#37b24d", "#f59f00", "#e64980", "#15aabf", "#7950f2", "#fa5252", "#82c91e", "#1098ad", "#d6336c", "#5c7cfa", "#94d82d"};

    const auto sheet = [&](const bool byQuality, const std::string &name) {
      Svg out(spanX * pixelsPerMetre + 80, spanZ * pixelsPerMetre + 80, pixelsPerMetre);
      out.rect(-1e5, -1e5, 2e5, 2e5, "#11141a");

      for (uint32_t f = 0; f < grid.mesh.faces(); ++f)
      {
        geom2::Point corner[4];
        grid.mesh.corners(f, corner);

        Poly poly;

        for (uint32_t i = 0; i < 4; ++i)
        {
          poly.push_back({geom2::toMetres(corner[i].x) - geom2::toMetres(box.minX) + 20, 0, geom2::toMetres(corner[i].y) - geom2::toMetres(box.minY) + 20});
        }

        std::string fill = patchPalette[grid.hexOfFace[f] % 12];

        if (byQuality)
        {
          // Green where the cell is close to the target edge, red where it is
          // not: a bad region is visible instead of inferred from a percentile.
          double shortest = 1e9, longest = 0;

          for (uint32_t i = 0; i < 4; ++i)
          {
            const double dx = geom2::toMetres(corner[(i + 1) % 4].x - corner[i].x);
            const double dz = geom2::toMetres(corner[(i + 1) % 4].y - corner[i].y);
            const double length = std::sqrt(dx * dx + dz * dz);
            shortest = std::min(shortest, length);
            longest = std::max(longest, length);
          }

          const double aspect = shortest > 0 ? longest / shortest : 4;
          const int bad = int(std::clamp((aspect - 1.0) / 1.2, 0.0, 1.0) * 255);

          char buffer[16];
          std::snprintf(buffer, sizeof(buffer), "#%02x%02x40", bad, 220 - bad);
          fill = buffer;
        }

        out.polygon(poly, fill, "#e9ecef", .35, .6);
      }

      const auto ring = [&](const geom2::Ring &r) {
        Poly poly;

        for (const geom2::Point &pt : r)
        {
          poly.push_back({geom2::toMetres(pt.x) - geom2::toMetres(box.minX) + 20, 0, geom2::toMetres(pt.y) - geom2::toMetres(box.minY) + 20});
        }

        poly.push_back(poly.front());
        out.polyline(poly, "#ffd43b", 1.4);
      };

      ring(grid.region.outer);

      for (const geom2::Ring &hole : grid.region.holes)
      {
        ring(hole);
      }

      std::ofstream(output / name) << out.finish();
    };

    sheet(false, "city-wfc-grid.svg");
    sheet(true, "city-wfc-quality.svg");

    // Milestone two: wall, gates, roads, river, blocks, over the same grid.
    CityLabelInput planInput;
    planInput.centre = {site.localCentre.x, 0, site.localCentre.z};
    planInput.terrain = [&](const double x, const double z) { return site.brief.terrain ? site.brief.terrain(x, z) : 0.0; };

    if (site.water)
    {
      planInput.water = site.water;
    }

    // The world's own road exits: where the regional roads meet this
    // settlement. Gates are placed on the wall nearest each of them, so the
    // city's gates line up with roads that already exist.
    for (const RuralSource &exit : site.brief.input.ruralSources)
    {
      planInput.exits.push_back(exit.exit);
    }

    CityLabelSettings planSettings;
    CityPlan plan;
    std::string planError;

    const auto labelStarted = std::chrono::steady_clock::now();
    const bool planOK = labelCity(grid, planInput, planSettings, plan, &planError);
    const double labelMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - labelStarted).count();

    if (!planOK)
    {
      std::cout << "city plan rejected: " << planError << "\n";
    }
    else
    {
      const CityLabelMetrics &m = plan.metrics;

      std::cout << "CITY_WFC_LABEL seed=" << settings.brief.seed
                << " site=" << site.regionName
                << " cells=" << m.cells
                << " wall=" << m.wall << " towers=" << m.towers << " gates=" << m.gates
                << " road=" << m.road << " bridge=" << m.bridge << " ford=" << m.ford << " steps=" << m.steps
                << " water=" << m.water << " plaza=" << m.plaza << " block=" << m.block
                << " blocks=" << m.blocks << " landlocked=" << m.landlocked << " unreachable=" << m.unreachable << " alleys=" << m.alleys
                << " dropped=" << m.droppedGates
                << " passage=" << m.minimumPassageMetres
                << " layers=" << int(m.maximumLayer)
                << " outlines=" << plan.outlines.size()
                << " unassigned=" << m.unassigned
                << " ms=" << labelMilliseconds << "\n";

      for (const std::string &entry : plan.ledger)
      {
        std::cout << "  plan: " << entry << "\n";
      }

      const auto kindColour = [](const CellKind kind) -> const char * {
        switch (kind)
        {
        case CellKind::Water:
          return "#1b6ca8";
        case CellKind::Ford:
          return "#5ba4cf";
        case CellKind::Wall:
          return "#4a4a52";
        case CellKind::Tower:
          return "#23232a";
        case CellKind::Gate:
          return "#e03131";
        case CellKind::Road:
          return "#b0a99f";
        case CellKind::Bridge:
          return "#8c6d46";
        case CellKind::Steps:
          return "#d9a441";
        case CellKind::Plaza:
          return "#efe6d5";
        case CellKind::Block:
          return "#6c9a5f";
        default:
          return "#ff00ff";
        }
      };

      Svg label(spanX * pixelsPerMetre + 80, spanZ * pixelsPerMetre + 80, pixelsPerMetre);
      label.rect(-1e5, -1e5, 2e5, 2e5, "#11141a");

      static const char *blockTints[] = {"#6c9a5f", "#7fa86a", "#5e8c55", "#88ab72", "#4f7d4a", "#93b57d"};

      const auto shift = [&](const geom2::Point &p) {
        return Position{geom2::toMetres(p.x) - geom2::toMetres(box.minX) + 20, 0, geom2::toMetres(p.y) - geom2::toMetres(box.minY) + 20};
      };

      for (uint32_t f = 0; f < grid.mesh.faces(); ++f)
      {
        geom2::Point corner[4];
        grid.mesh.corners(f, corner);

        Poly poly;

        for (uint32_t i = 0; i < 4; ++i)
        {
          poly.push_back(shift(corner[i]));
        }

        const CityCell &cell = plan.cells[f];
        std::string fill = kindColour(cell.kind);

        if (cell.kind == CellKind::Block && cell.block != kNoBlock)
        {
          fill = blockTints[cell.block % 6];
        }

        label.polygon(poly, fill, "#0b0d11", .2);
      }

      for (const CityOutline &outline : plan.outlines)
      {
        if (outline.role == OutlineRole::Block)
        {
          continue;
        }

        Poly poly;

        for (const Position &p : outline.loop)
        {
          poly.push_back({p.x - geom2::toMetres(box.minX) + 20, 0, p.z - geom2::toMetres(box.minY) + 20});
        }

        poly.push_back(poly.front());
        label.polyline(poly, outline.role == OutlineRole::Wall ? "#ffffff" : (outline.role == OutlineRole::Road ? "#f1e5c8" : "#7fd4ff"), .7);
      }

      for (const CityRoute &route : plan.routes)
      {
        Poly poly;

        for (const Position &p : route.line)
        {
          poly.push_back({p.x - geom2::toMetres(box.minX) + 20, 0, p.z - geom2::toMetres(box.minY) + 20});
        }

        label.polyline(poly, "#ffd43b", .9);
      }

      std::ofstream(output / "city-wfc-label.svg") << label.finish();

      report["cityWfcLabel"] = {
        {"wall", m.wall}, {"towers", m.towers}, {"gates", m.gates},
        {"road", m.road}, {"bridge", m.bridge}, {"ford", m.ford}, {"steps", m.steps},
        {"water", m.water}, {"plaza", m.plaza}, {"block", m.block},
        {"blocks", m.blocks}, {"landlocked", m.landlocked}, {"unreachable", m.unreachable}, {"alleys", m.alleys},
        {"droppedGates", m.droppedGates}, {"unassigned", m.unassigned},
        {"minimumPassage", m.minimumPassageMetres}, {"maximumLayer", m.maximumLayer},
        {"outlines", plan.outlines.size()}, {"milliseconds", labelMilliseconds},
        {"ledger", plan.ledger}};
    }

    report["cityWfc"] = {
      {"faces", q.faces},
      {"vertices", q.vertices},
      {"hexes", grid.lattice.hexes},
      {"triangles", q.triangles},
      {"unpaired", q.unpairedTriangles},
      {"targetEdge", q.targetEdgeMetres},
      {"edgeP05", q.p05Edge}, {"edgeMedian", q.medianEdge}, {"edgeP95", q.p95Edge},
      {"areaP05", q.p05Area}, {"areaMedian", q.medianArea}, {"areaP95", q.p95Area},
      {"angleP05", q.p05InteriorAngle}, {"angleMin", q.minInteriorAngle},
      {"aspectP95", q.p95Aspect},
      {"coverage", q.coverageRatio},
      {"deviation", q.boundaryDeviationMetres},
      {"residualRatio", q.residualRatio},
      {"peeled", q.mergedRim},
      {"milliseconds", milliseconds},
      {"ledger", grid.ledger}};

    std::ofstream(output / "settlement.json") << report.dump(2);

    return 0;
  }

  struct VoxelAssemblyAudit {
    struct SourceCounts {
      uint32_t pieces=0, rooms=0, roomConnections=0, windows=0;
      double base=0, minX=0, maxX=0, minZ=0, maxZ=0;
    };
    uint64_t dwellings=0, parts=0, faces=0, facades=0, connectors=0;
    uint64_t legacyTriangles=0, legacyPanels=0;
    uint64_t unaligned=0, uniqueOrientations=0, bakedPlacements=0, uniqueMeshes=0, uniqueTriangles=0;
    uint64_t partyParts=0, foundationJoins=0, foundationSamples=0;
    uint64_t sampledDwellings=0, sampledParts=0, sampledCells=0, sampledClipped=0;
    uint64_t sampledBlocks=0,sampledFarCells=0;
    uint64_t sampledCoveredStaticParts=0;
    uint64_t sampledPairedFacadeSides=0;
    uint64_t sampledDecorativeOverlays=0, sampledOverlayCells=0;
    uint64_t sampledTerrainTiles=0, sampledTerrainCells=0;
    uint64_t boundaryFeatures=0, partySeams=0, sharedPartySeams=0, partyContacts=0, terrainBorderChecks=0;
    // The sink claims the one native-dwelling-sources.json without reading
    // sampledDwellings, which the voxel jobs write under assetsMutex.
    std::atomic_flag sourcesWritten=ATOMIC_FLAG_INIT;
    std::unordered_set<uint64_t> covered;
    std::unordered_set<uint64_t> dwellingIds;
    std::unordered_set<uint64_t> geometryKeys;
    std::unordered_set<uint64_t> appearanceKeys;
    std::vector<std::pair<uint64_t,uint64_t>> triangleSpans;
    std::unordered_map<uint64_t,SourceCounts> sourceCounts;
  };
  auto voxelAudit=std::make_shared<VoxelAssemblyAudit>();
  auto nativeAssets=std::make_shared<voxel::svraster::CityVoxelAssetCompiler>();
  struct NativeCityOwner {
    uint64_t id=0;
    std::array<int64_t,3> originMillimetres{};
    uint16_t yawCode=0;
    // One list per quarter turn: a block turned by t sits in the owner frame
    // yawed by another t quarters.
    std::array<std::vector<virtualgeometry::CityVoxelCityAssembler::Part>,4> parts;
    // The whole house at 4x the pitch, drawn instead of its blocks far away.
    virtualgeometry::CityVoxelCityAssembler::Part far{};
    size_t partCount() const { return parts[0].size()+parts[1].size()+parts[2].size()+parts[3].size(); }
  };
  struct NativeCityExport {
    std::vector<NativeCityOwner> owners;
    std::set<std::pair<int32_t,int32_t>> terrainTiles;
    uint64_t terrainCells=0;
    uint64_t baseCells=0,baseTiles=0;
    std::string sceneError;
  };
  auto nativeCity=std::make_shared<NativeCityExport>();
  // Dwelling voxelisation runs beside city generation. Slicing reads only its
  // own copies; asset dedupe and the export tables take `assetsMutex`. Asset
  // IDs do not depend on submission order and owners are sorted before the
  // scene is written, so the output is the same for any worker count.
  struct NativeDwellingQueue {
    std::mutex mutex,assetsMutex;
    std::condition_variable ready,room,idle;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> threads;
    size_t running=0,limit=1;
    uint64_t queued=0;
    bool stopping=false;
    std::string firstError;
    struct Sweep {
      virtualgeometry::CityVoxelDwellingBlockSettings settings;
      voxel::svraster::CityVoxelAssetCompiler assets;
      uint64_t blocks=0;
    };
    std::deque<Sweep> sweep; // Fixed before the first job; guarded by assetsMutex after.
    std::unordered_set<uint64_t> houseShapes;
    std::vector<std::pair<uint64_t,std::string>> failedHouses;
    uint64_t civilOwners=0,civilCells=0; // Guarded by assetsMutex.
    uint64_t houseCells=0;
    voxel::svraster::CityVoxelAssetCompiler farAssets;
    std::ofstream blockDump;
    uint64_t farCells=0;
    uint32_t farPitch=0;
    explicit NativeDwellingQueue(unsigned workers) {
      workers=std::max(1u,workers);
      limit=size_t(workers)*2;
      for(unsigned i=0;i<workers;++i)threads.emplace_back([this] {
        for(;;) {
          std::function<void()> job;
          {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock,[&]{return stopping||!jobs.empty();});
            if(jobs.empty())return;
            job=std::move(jobs.front());jobs.pop_front();++running;
          }
          room.notify_one();
          job();
          std::lock_guard<std::mutex> lock(mutex);
          --running;
          room.notify_one();
          if(jobs.empty()&&!running)idle.notify_all();
        }
      });
    }
    ~NativeDwellingQueue() {
      { std::lock_guard<std::mutex> lock(mutex); stopping=true; }
      ready.notify_all();
      for(auto &thread:threads)thread.join();
    }
    void push(std::function<void()> job) {
      std::unique_lock<std::mutex> lock(mutex);
      room.wait(lock,[&]{return jobs.size()+running<limit;});
      jobs.push_back(std::move(job));
      ready.notify_one();
    }
    void drain() {
      std::unique_lock<std::mutex> lock(mutex);
      idle.wait(lock,[&]{return jobs.empty()&&!running;});
    }
    void fail(const std::string &message) {
      std::lock_guard<std::mutex> lock(mutex);
      if(firstError.empty())firstError=message;
    }
    std::string failure() {
      std::lock_guard<std::mutex> lock(mutex);
      return firstError;
    }
  };
  auto nativeQueue=std::make_shared<NativeDwellingQueue>(
      std::max(1u,std::thread::hardware_concurrency()-1));
  if(!voxelBlockDump.empty()) {
    nativeQueue->blockDump.open(voxelBlockDump);
    nativeQueue->blockDump<<"dwelling,bx,by,bz,turn,geometry,appearance,cells,voxelsPerCell\n";
  }
  for(size_t at=0;at<voxelBlockSweep.size();) {
    const size_t end=std::min(voxelBlockSweep.find(',',at),voxelBlockSweep.size());
    const std::string item=voxelBlockSweep.substr(at,end-at);
    const size_t x=item.find('x');
    if(x==std::string::npos){std::cerr<<"--voxel-block-sweep wants ACROSSxUP,...\n";return 1;}
    nativeQueue->sweep.emplace_back();
    nativeQueue->sweep.back().settings={uint32_t(std::stoul(item.substr(0,x))),
                                        uint32_t(std::stoul(item.substr(x+1)))};
    at=end+1;
  }
  if(voxelAssemblyAudit) {
    cityMeshSettings.voxelAssemblySink=[voxelAudit,nativeAssets,nativeCity,nativeQueue,voxelNativeSample,voxelNativeSampleLimit,voxelNativeExport,voxelBlocks,voxelFarFactor,output](const hierarchy::DwellingVoxelAssembly &assembly,
                                                   const hierarchy::Skeleton &skeleton,std::string *error) {
      if(!hierarchy::auditDwellingVoxelAssembly(assembly,skeleton,error))return false;
      if(voxelNativeExport && !voxelAudit->sourcesWritten.test_and_set(std::memory_order_relaxed)) {
        json source={{"dwellingId",assembly.dwellingId},
                     {"pieceCount",skeleton.pieces.size()},
                     {"facadeCount",skeleton.structure.instances.size()},
                     {"frame",{{"originX",assembly.grid.frame.originX},
                                {"originZ",assembly.grid.frame.originZ},
                                {"axisX",assembly.grid.frame.axisX},
                                {"axisZ",assembly.grid.frame.axisZ}}},
                     {"minimumVoxel",assembly.grid.minimumVoxel},
                     {"maximumVoxel",assembly.grid.maximumVoxel}};
        source["facades"]=json::array();
        for(uint32_t i=0;i<skeleton.structure.instances.size();++i) {
          const auto &facade=skeleton.structure.instances[i];
          source["facades"].push_back({{"index",i},{"asset",uint32_t(facade.asset)},
              {"segment",facade.segment},{"inside",facade.inside},
              {"supportFace",facade.supportFace},
              {"origin",{facade.origin.x,facade.origin.y,facade.origin.z}},
              {"width",facade.width},{"height",facade.height},{"depth",facade.depth}});
        }
        std::ofstream(output/"native-dwelling-sources.json")<<source.dump(2);
      }
      if(voxelNativeSample && (!voxelNativeSampleLimit ||
                               nativeQueue->queued<voxelNativeSampleLimit)) {
        if(!nativeQueue->failure().empty()) {
          if(error)*error=nativeQueue->failure();
          return false;
        }
        auto exportAssembly=std::make_shared<hierarchy::DwellingVoxelAssembly>(assembly);
        if(voxelNativeExport) {
          const double ratio=assembly.grid.voxelMetres/.1;
          exportAssembly->grid.voxelMetres=.1;
          for(unsigned axis=0;axis<3;++axis) {
            exportAssembly->grid.minimumVoxel[axis]=int32_t(std::floor(
                double(assembly.grid.minimumVoxel[axis])*ratio))-2;
            exportAssembly->grid.maximumVoxel[axis]=int32_t(std::ceil(
                double(assembly.grid.maximumVoxel[axis])*ratio))+2;
          }
        }
        // The skeleton dies when this sink returns; the job owns a copy.
        auto source=std::make_shared<hierarchy::Skeleton>(skeleton);
        ++nativeQueue->queued;
        nativeQueue->push([voxelAudit,nativeAssets,nativeCity,nativeQueue,voxelNativeExport,
                           voxelBlocks,voxelFarFactor,exportAssembly,source] {
          const auto &assembly=*exportAssembly;
          std::vector<virtualgeometry::CityVoxelCanonicalBlock> blocks;
          uint32_t pitch=0;
          virtualgeometry::CityVoxelDwellingCompileReport sampled;
          std::string jobError;
          std::vector<voxel::svraster::CityVoxelCell> houseCells;
          const auto blockSide=[&](uint32_t value){return int32_t(value?value:assembly.grid.voxelsPerCell);};
          bool sliced=virtualgeometry::resolveCityVoxelDwellingCells(assembly,*source,
              houseCells,pitch,sampled,&jobError) &&
              virtualgeometry::sliceCityVoxelBlocks(houseCells,blockSide(voxelBlocks.horizontalVoxels),
                  blockSide(voxelBlocks.verticalVoxels),blocks,&jobError);
          // Alternative block sizes for the sharing report, sliced from the
          // same resolved cells.
          std::vector<std::vector<virtualgeometry::CityVoxelCanonicalBlock>> sweepBlocks(
              nativeQueue->sweep.size());
          for(size_t i=0;sliced && i<sweepBlocks.size();++i)
            sliced=virtualgeometry::sliceCityVoxelBlocks(houseCells,
                blockSide(nativeQueue->sweep[i].settings.horizontalVoxels),
                blockSide(nativeQueue->sweep[i].settings.verticalVoxels),sweepBlocks[i],&jobError);
          uint64_t houseHash=14695981039346656037ull;
          if(sliced) {
            std::sort(houseCells.begin(),houseCells.end(),[](const auto &a,const auto &b){
              return a.position<b.position;});
            for(const auto &cell:houseCells)
              for(const uint32_t v:{uint32_t(cell.position[0]),uint32_t(cell.position[1]),
                                    uint32_t(cell.position[2]),cell.rgba,cell.materialProfile})
                houseHash=(houseHash^v)*1099511628211ull;
          }
          std::lock_guard<std::mutex> lock(nativeQueue->assetsMutex);
          for(size_t i=0;sliced && i<sweepBlocks.size();++i) {
            std::vector<virtualgeometry::CityVoxelCompiledDwellingBlock> ignored;
            auto &entry=nativeQueue->sweep[i];
            sliced=virtualgeometry::submitCityVoxelDwellingBlocks(assembly.dwellingId,pitch,
                sweepBlocks[i],entry.assets,ignored,&jobError);
            entry.blocks+=sweepBlocks[i].size();
          }
          nativeQueue->houseShapes.insert(houseHash);
          nativeQueue->houseCells+=houseCells.size();
          std::vector<voxel::svraster::CityVoxelCell> farCells;
          voxel::svraster::CityVoxelCompiledPlacement farPlacement{};
          if(sliced) {
            const int32_t farFactor=voxelFarFactor;
            virtualgeometry::downsampleCityVoxelCells(houseCells,farFactor,farCells);
            uint64_t farId=(assembly.dwellingId^0x464152ull)*1099511628211ull;
            if(!farId)farId=1;
            sliced=nativeQueue->farAssets.submit(farId,pitch*farFactor,farCells,farPlacement,&jobError);
            nativeQueue->farCells+=farCells.size();
            nativeQueue->farPitch=pitch*farFactor;
          }
          std::vector<virtualgeometry::CityVoxelCompiledDwellingBlock> placements;
          if(!sliced || !virtualgeometry::submitCityVoxelDwellingBlocks(assembly.dwellingId,pitch,
                blocks,*nativeAssets,placements,&jobError)) {
            // One house the voxel compiler cannot represent must not cost the
            // city: it is named in the report and left out of the scene.
            nativeQueue->failedHouses.push_back({assembly.dwellingId,jobError});
            return;
          }
          voxelAudit->sampledBlocks+=placements.size();
          if(nativeQueue->blockDump.is_open())
            for(size_t i=0;i<placements.size();++i) {
              const auto &b=placements[i];
              nativeQueue->blockDump<<assembly.dwellingId<<','<<b.blockCell[0]<<','<<b.blockCell[1]<<','
                  <<b.blockCell[2]<<','<<int(b.quarterTurn)<<','<<b.placement.geometryId<<','
                  <<b.placement.appearanceId<<','<<blocks[i].cells.size()<<','
                  <<assembly.grid.voxelsPerCell<<'\n';
            }
          voxelAudit->sampledFarCells+=farCells.size();
          ++voxelAudit->sampledDwellings;
          voxelAudit->sampledParts+=placements.size();
          voxelAudit->sampledCells+=sampled.ownership.uniqueCells;
          voxelAudit->sampledClipped+=sampled.ownership.clippedCells;
          voxelAudit->sampledCoveredStaticParts+=sampled.omittedCoveredStaticParts;
          voxelAudit->sampledPairedFacadeSides+=sampled.omittedPairedFacadeSides;
          voxelAudit->sampledDecorativeOverlays+=sampled.omittedDecorativeOverlays;
          voxelAudit->sampledOverlayCells+=sampled.overlaidCells;
          if(!voxelNativeExport)return;
          NativeCityOwner owner;
          owner.id=assembly.dwellingId;
          owner.originMillimetres={int64_t(std::llround(assembly.grid.frame.originX*1000.0)),
              int64_t(std::llround(assembly.grid.baseMetres*1000.0)),
              int64_t(std::llround(assembly.grid.frame.originZ*1000.0))};
          constexpr double tau=6.2831853071795864769;
          double angle=std::atan2(assembly.grid.frame.axisZ,assembly.grid.frame.axisX);
          if(angle<0)angle+=tau;
          owner.yawCode=uint16_t(std::llround(angle*65536.0/tau));
          for(const auto &entry:placements)
            owner.parts[entry.quarterTurn].push_back({entry.placement.stableId,
                entry.placement.geometryId,entry.placement.appearanceId,entry.frameOrigin});
          owner.far={farPlacement.stableId,farPlacement.geometryId,farPlacement.appearanceId,
                     farPlacement.originCell};
          nativeCity->owners.push_back(std::move(owner));
          double loX=INFINITY,loZ=INFINITY,hiX=-INFINITY,hiZ=-INFINITY;
          for(const int32_t x:{assembly.grid.minimumVoxel[0],assembly.grid.maximumVoxel[0]})
            for(const int32_t z:{assembly.grid.minimumVoxel[2],assembly.grid.maximumVoxel[2]}) {
              const double u=double(x)*assembly.grid.voxelMetres;
              const double v=double(z)*assembly.grid.voxelMetres;
              const double wx=assembly.grid.frame.originX+
                  u*assembly.grid.frame.axisX-v*assembly.grid.frame.axisZ;
              const double wz=assembly.grid.frame.originZ+
                  u*assembly.grid.frame.axisZ+v*assembly.grid.frame.axisX;
              loX=std::min(loX,wx);hiX=std::max(hiX,wx);
              loZ=std::min(loZ,wz);hiZ=std::max(hiZ,wz);
            }
          constexpr double tileMetres=32.0;
          for(int32_t tx=int32_t(std::floor((loX-1.0)/tileMetres));
              tx<=int32_t(std::floor((hiX+1.0)/tileMetres));++tx)
            for(int32_t tz=int32_t(std::floor((loZ-1.0)/tileMetres));
                tz<=int32_t(std::floor((hiZ+1.0)/tileMetres));++tz)
              nativeCity->terrainTiles.insert({tx,tz});
        });
      }
      if(!voxelAudit->dwellingIds.insert(assembly.dwellingId).second) {
        if(error)*error="duplicate voxel building ID";
        return false;
      }
      voxelAudit->covered.insert(assembly.dwellingId);
      VoxelAssemblyAudit::SourceCounts counts;
      counts.pieces=assembly.sourcePieceCount;counts.rooms=assembly.sourceRoomCount;
      counts.roomConnections=assembly.sourceRoomConnectionCount;counts.base=assembly.grid.baseMetres;
      counts.windows=assembly.plannedExteriorWindowCount;
      counts.minX=counts.minZ=1e100;counts.maxX=counts.maxZ=-1e100;
      for(const auto u:{assembly.grid.minimumVoxel[0],assembly.grid.maximumVoxel[0]})
        for(const auto v:{assembly.grid.minimumVoxel[2],assembly.grid.maximumVoxel[2]}) {
          double x=0,z=0;
          procedural::geom2::framePoint(assembly.grid.frame,double(u)/assembly.grid.voxelsPerCell,
                            double(v)/assembly.grid.voxelsPerCell,x,z);
          counts.minX=std::min(counts.minX,x);counts.maxX=std::max(counts.maxX,x);
          counts.minZ=std::min(counts.minZ,z);counts.maxZ=std::max(counts.maxZ,z);
        }
      voxelAudit->sourceCounts.emplace(assembly.dwellingId,counts);
      ++voxelAudit->dwellings;voxelAudit->parts+=assembly.parts.size();
      voxelAudit->faces+=assembly.sourceFaceCount;voxelAudit->facades+=assembly.sourceFacadeCount;
      voxelAudit->connectors+=assembly.connectors.size();
      voxelAudit->legacyTriangles+=assembly.legacyTriangleCount;
      voxelAudit->legacyPanels+=assembly.legacyPanelCount;
      if(assembly.legacyTriangleCount)
        voxelAudit->triangleSpans.push_back({assembly.legacyTriangleFirst,
          assembly.legacyTriangleFirst+assembly.legacyTriangleCount});
      voxelAudit->unaligned+=assembly.unalignedPlacementCount;
      voxelAudit->uniqueOrientations+=assembly.uniqueOrientationCount;
      for(const auto &part:assembly.parts) {
        voxelAudit->geometryKeys.insert(part.geometryKey);
        voxelAudit->appearanceKeys.insert(part.appearanceKey);
        voxelAudit->bakedPlacements+=part.requiresBakedGeometry;
        voxelAudit->partyParts+=part.partyWall;
      }
      return true;
    };
    cityMeshSettings.voxelUniqueMeshSink=[voxelAudit,nativeAssets,nativeCity,nativeQueue,voxelNativeExport,voxelFarFactor](
        const CityVoxelUniqueMeshPart &part,const CityModelPlan &model,std::string *error) {
      if(part.triangleCount==0 || part.firstTriangle>model.mesh.triangles.size() ||
         part.triangleCount>model.mesh.triangles.size()-part.firstTriangle ||
         model.materials.size()!=model.mesh.triangles.size()) {
        if(error)*error="invalid unique building triangle span";
        return false;
      }
      voxelAudit->covered.insert(part.buildingId);
      ++voxelAudit->uniqueMeshes;voxelAudit->uniqueTriangles+=part.triangleCount;
      voxelAudit->triangleSpans.push_back({part.firstTriangle,part.firstTriangle+part.triangleCount});
      if(!voxelNativeExport)return true;
      // Everything the city mesh holds that is not a cell dwelling - walls,
      // towers, plazas, trees, fences, bridges - becomes owners of its own:
      // voxelised at 10 cm in a frame turned to its longest edges, cut into
      // 32 m tiles so a curtain wall streams in pieces, then sliced into
      // blocks and given a far proxy exactly like a house.
      auto triangles=std::make_shared<std::vector<std::array<std::array<double,3>,3>>>();
      auto colours=std::make_shared<std::vector<uint32_t>>();
      triangles->reserve(part.triangleCount);colours->reserve(part.triangleCount);
      for(size_t t=part.firstTriangle;t<part.firstTriangle+part.triangleCount;++t) {
        std::array<std::array<double,3>,3> corners;
        for(int k=0;k<3;++k) {
          const auto &p=model.mesh.vertices[model.mesh.triangles[t][k]];
          corners[k]={p.x,p.y,p.z};
        }
        triangles->push_back(corners);
        const uint32_t surface=t<model.surfaceMaterials.size()?model.surfaceMaterials[t]:0u;
        colours->push_back(cityMeshVoxelColour(model.materials[t],surface));
      }
      const uint64_t partId=part.buildingId;
      const std::string role=part.role;
      ++nativeQueue->queued;
      nativeQueue->push([nativeAssets,nativeCity,nativeQueue,voxelFarFactor,triangles,colours,partId,role] {
        constexpr double voxel=.1,tau=6.2831853071795864769;
        constexpr int32_t tileVoxels=320;
        const uint16_t yawCode=uint16_t(int64_t(std::llround(
            virtualgeometry::cityVoxelMeshPlanYaw(*triangles)*65536.0/tau))&0xffff);
        const double yaw=double(yawCode)*tau/65536.0,ax=std::cos(yaw),az=std::sin(yaw);
        double lo[3]={INFINITY,INFINITY,INFINITY};
        for(const auto &t:*triangles)for(const auto &p:t) {
          lo[0]=std::min(lo[0],p[0]*ax+p[2]*az);
          lo[1]=std::min(lo[1],p[1]);
          lo[2]=std::min(lo[2],-p[0]*az+p[2]*ax);
        }
        const double ou=std::floor(lo[0]/voxel)*voxel,ov=std::floor(lo[2]/voxel)*voxel;
        const std::array<int64_t,3> originMm{int64_t(std::llround((ou*ax-ov*az)*1000.0)),
            int64_t(std::llround(std::floor(lo[1]/voxel)*voxel*1000.0)),int64_t(std::llround((ou*az+ov*ax)*1000.0))};
        const double ox=originMm[0]*1e-3,oy=originMm[1]*1e-3,oz=originMm[2]*1e-3;
        std::map<std::pair<int32_t,int32_t>,std::vector<virtualgeometry::CityVoxelMeshTriangle>> tiles;
        for(size_t i=0;i<triangles->size();++i) {
          virtualgeometry::CityVoxelMeshTriangle triangle;
          double cu=0,cv=0;
          for(int k=0;k<3;++k) {
            const auto &p=(*triangles)[i][k];
            const double dx=p[0]-ox,dz=p[2]-oz;
            triangle.corners[k]={(dx*ax+dz*az)/voxel,(p[1]-oy)/voxel,(-dx*az+dz*ax)/voxel};
            cu+=triangle.corners[k][0]/3;cv+=triangle.corners[k][2]/3;
          }
          triangle.rgba=(*colours)[i];
          tiles[{int32_t(std::floor(cu/tileVoxels)),int32_t(std::floor(cv/tileVoxels))}].push_back(triangle);
        }
        for(const auto &[tile,tileTriangles]:tiles) {
          uint64_t ownerId=0xcbf29ce484222325ull;
          for(const uint64_t v:{partId,uint64_t(uint32_t(tile.first)),uint64_t(uint32_t(tile.second))})
            ownerId=(ownerId^v)*0x100000001b3ull;
          ownerId|=1ull<<63;
          std::vector<voxel::svraster::CityVoxelCell> cells;
          std::vector<virtualgeometry::CityVoxelCanonicalBlock> blocks;
          std::string jobError;
          constexpr uint32_t pitch=100000u;
          bool ok=virtualgeometry::voxelizeCityVoxelMesh(tileTriangles,true,8'000'000u,cells,nullptr,&jobError) &&
                  !cells.empty() && virtualgeometry::sliceCityVoxelBlocks(cells,16,16,blocks,&jobError);
          if(!ok && cells.empty() && jobError.empty())continue; // Nothing with area.
          std::lock_guard<std::mutex> lock(nativeQueue->assetsMutex);
          std::vector<voxel::svraster::CityVoxelCell> farCells;
          voxel::svraster::CityVoxelCompiledPlacement farPlacement{};
          if(ok) {
            virtualgeometry::downsampleCityVoxelCells(cells,voxelFarFactor,farCells);
            uint64_t farId=(ownerId^0x464152ull)*1099511628211ull;
            if(!farId)farId=1;
            ok=nativeQueue->farAssets.submit(farId,pitch*voxelFarFactor,farCells,farPlacement,&jobError);
          }
          std::vector<virtualgeometry::CityVoxelCompiledDwellingBlock> placements;
          if(!ok || !virtualgeometry::submitCityVoxelDwellingBlocks(ownerId,pitch,blocks,*nativeAssets,placements,&jobError)) {
            nativeQueue->failedHouses.push_back({ownerId,role+": "+jobError});
            continue;
          }
          NativeCityOwner owner;
          owner.id=ownerId;
          owner.originMillimetres=originMm;
          owner.yawCode=yawCode;
          for(const auto &entry:placements)
            owner.parts[entry.quarterTurn].push_back({entry.placement.stableId,
                entry.placement.geometryId,entry.placement.appearanceId,entry.frameOrigin});
          owner.far={farPlacement.stableId,farPlacement.geometryId,farPlacement.appearanceId,farPlacement.originCell};
          nativeCity->owners.push_back(std::move(owner));
          ++nativeQueue->civilOwners;
          nativeQueue->civilCells+=cells.size();
        }
      });
      return true;
    };
    cityMeshSettings.voxelFoundationSink=[voxelAudit](uint64_t,
        const std::vector<CityVoxelFoundationSample> &samples,std::string *error) {
      if(samples.empty()) { if(error)*error="empty voxel foundation join"; return false; }
      for(const auto &sample:samples)if(sample.groundMillimetres>sample.topMillimetres) {
        if(error)*error="foundation relief penetrates support top"; return false;
      }
      ++voxelAudit->foundationJoins;
      voxelAudit->foundationSamples+=samples.size();
      return true;
    };
    cityMeshSettings.voxelBoundarySink=[voxelAudit,nativeAssets,nativeCity,nativeQueue,voxelNativeSample,voxelNativeExport,voxelNativeSampleLimit,cityGroundOverlay,voxelTracePages,&site,&world,output](const CityVoxelBoundaryPlan &boundary,
        const CityModelPlan &model,const CityCompletionPlan &,const CityReliefPlan *relief,
        const std::function<double(double,double)> &ground,std::string *error) {
      if(!auditCityVoxelBoundaryPlan(boundary,error))return false;
      nativeQueue->drain();
      if(!nativeQueue->failure().empty()) {
        if(error)*error=nativeQueue->failure();
        return false;
      }
      std::sort(nativeCity->owners.begin(),nativeCity->owners.end(),
          [](const NativeCityOwner &a,const NativeCityOwner &b){return a.id<b.id;});
      if(voxelNativeSample) {
        virtualgeometry::CityVoxelTerrainTileSpec tile;
        tile.tileX=boundary.terrain.minTileX;
        tile.tileZ=boundary.terrain.minTileZ;
        tile.cellsPerSide=CityVoxelTileGrid::voxelsPerTile;
        std::vector<voxel::svraster::CityVoxelCell> cells;
        if(!virtualgeometry::sampleCityVoxelTerrainTile(tile,ground,cells,error))return false;
        ++voxelAudit->sampledTerrainTiles;
        voxelAudit->sampledTerrainCells+=cells.size();
      }
      const bool exportScene=voxelNativeExport && !nativeCity->owners.empty();
      if(exportScene || cityGroundOverlay) {
        constexpr uint32_t scenePitchMicrometres=100000u;
        const CityGroundWeightIndex groundPaint=buildCityGroundWeightIndex(model.surfaces);
        std::vector<virtualgeometry::CityVoxelBasePrism> basePrisms;
        const auto polygonOf=[](const std::vector<Position> &ring) {
          procedural::geom2::Polygon polygon;
          for(const auto &p:ring)polygon.outer.push_back({procedural::geom2::toUnit(p.x),procedural::geom2::toUnit(p.z)});
          if(!polygon.empty()&&!procedural::geom2::counterClockwise(polygon.outer))procedural::geom2::reverse(polygon.outer);
          return polygon;
        };
        if(relief) {
          for(const auto &deck:relief->platforms) {
            virtualgeometry::CityVoxelBasePrism prism;
            prism.footprint=polygonOf(deck.boundary);
            prism.bottomMetres=deck.foundation;
            prism.topMetres=deck.level+.04;
            prism.topMaterial=deck.rockCut?19:37;
            prism.sideMaterial=deck.rockCut?27:54;
            prism.topRgba=deck.rockCut?0xff81898bu:0xff879098u;
            prism.sideRgba=deck.rockCut?0xff616d74u:0xff5d6469u;
            if(!prism.footprint.empty())basePrisms.push_back(std::move(prism));
          }
          for(const auto &landing:relief->stairLandings) {
            virtualgeometry::CityVoxelBasePrism prism;
            prism.footprint=polygonOf(landing.boundary);
            prism.bottomMetres=landing.foundation;
            prism.topMetres=landing.level;
            if(!prism.footprint.empty())basePrisms.push_back(std::move(prism));
          }
          for(const auto &stair:relief->stairs) {
            const double dx=stair.b.x-stair.a.x,dz=stair.b.z-stair.a.z;
            const double length=std::hypot(dx,dz);
            if(length<=0)continue;
            const double ux=dx/length,uz=dz/length;
            const auto point=[&](double along,double side) {
              return Position{stair.a.x+ux*along-uz*side,0,stair.a.z+uz*along+ux*side};
            };
            double previous=0;
            for(const auto &band:stair.bands) {
              virtualgeometry::CityVoxelBasePrism prism;
              prism.footprint=polygonOf({point(previous,-stair.halfWidth),
                  point(band.first,-stair.halfWidth),point(band.first,stair.halfWidth),
                  point(previous,stair.halfWidth)});
              prism.bottomMetres=stair.supported?stair.foundation:stair.a.y-.28;
              prism.topMetres=band.second;
              prism.topMaterial=37;prism.sideMaterial=36;
              prism.topRgba=0xff88939au;prism.sideRgba=0xff647077u;
              if(!prism.footprint.empty())basePrisms.push_back(std::move(prism));
              previous=band.first;
            }
          }
        }
        for(const auto &piece:model.surfaces.pieces) {
          if(!piece.terrainFollowing||piece.height<=piece.bottom+.049||piece.polygon.empty())continue;
          virtualgeometry::CityVoxelBasePrism prism;
          prism.footprint=piece.polygon;
          prism.bottomMetres=piece.bottom;
          prism.topMetres=piece.height;
          prism.terrainFollowing=true;
          const bool pavement=piece.kind==CitySurfaceKind::Sidewalk||
              piece.kind==CitySurfaceKind::StairTread;
          prism.topMaterial=pavement?(piece.owner%2?33u:18u):
              piece.kind==CitySurfaceKind::Court?19u:
              piece.pavingMaterial?piece.pavingMaterial:11u;
          prism.sideMaterial=54u;
          prism.topRgba=pavement?0xff8f9aa1u:0xff7d8b8du;
          prism.sideRgba=0xff606b70u;
          basePrisms.push_back(std::move(prism));
        }
        if(cityGroundOverlay) {
          // Runtime terrain reads this instead of baked tiles: graded ground
          // on a 1 m grid, unpaved paint on 0.5 m, paving as exact prisms.
          const auto started=std::chrono::steady_clock::now();
          voxel::svraster::CityGroundOverlayData overlay;
          constexpr int64_t margin=16000,tileMm=int64_t(CityVoxelTileGrid::voxelsPerTile)*
              CityVoxelTileGrid::millimetresPerVoxel;
          const int64_t minX=int64_t(boundary.terrain.minTileX)*tileMm-margin;
          const int64_t minZ=int64_t(boundary.terrain.minTileZ)*tileMm-margin;
          const int64_t maxX=int64_t(boundary.terrain.maxTileX)*tileMm+margin;
          const int64_t maxZ=int64_t(boundary.terrain.maxTileZ)*tileMm+margin;
          overlay.originMillimetres={minX,minZ};
          overlay.siteOriginMillimetres={int64_t(std::llround(site.origin.x*1000.0)),
                                         int64_t(std::llround(site.origin.z*1000.0))};
          overlay.groundSpacingMillimetres=1000;
          overlay.groundWidth=uint32_t((maxX-minX)/1000+1);
          overlay.groundHeight=uint32_t((maxZ-minZ)/1000+1);
          overlay.groundMillimetres.resize(size_t(overlay.groundWidth)*overlay.groundHeight);
          overlay.groundWeight.resize(overlay.groundMillimetres.size());
          // Datum check on the outer ring, where the city blends into the
          // map: the renderer's --city-ground-offset is map minus city.
          double datumSum=0,datumMin=INFINITY,datumMax=-INFINITY;uint64_t datumCount=0;
          // Rows go to all cores (the ground field is read concurrently by
          // the lot planner already); the datum is summed per row.
          struct DatumRow{double sum=0,minimum=INFINITY,maximum=-INFINITY;uint64_t count=0;};
          std::vector<DatumRow> datumRows(overlay.groundHeight);
          std::atomic<uint32_t> nextGroundRow{0};
          std::vector<std::thread> groundRows;
          for(unsigned t=0;t<std::max(1u,std::thread::hardware_concurrency());++t)
            groundRows.emplace_back([&] {
          for(uint32_t z;(z=nextGroundRow.fetch_add(1))<overlay.groundHeight;) {
            auto &row=datumRows[z];
            for(uint32_t x=0;x<overlay.groundWidth;++x) {
              const int64_t mx=minX+int64_t(x)*1000,mz=minZ+int64_t(z)*1000;
              const double h=ground(double(mx)*.001,double(mz)*.001);
              const size_t i=size_t(z)*overlay.groundWidth+x;
              overlay.groundMillimetres[i]=std::isfinite(h)?int32_t(std::llround(h*1000.0)):0;
              // Full weight inside the city tiles, ramping out over the margin.
              const int64_t inset=std::min({mx-minX,maxX-mx,mz-minZ,maxZ-mz});
              overlay.groundWeight[i]=std::isfinite(h)?
                  uint8_t(std::clamp<int64_t>(inset*255/margin,0,255)):0;
              if(std::isfinite(h) && inset<1000) {
                const double map=world.surface.sample({site.origin.x+double(mx)*.001,
                    site.origin.z+double(mz)*.001}).elevationMetres;
                row.sum+=map-h;++row.count;
                row.minimum=std::min(row.minimum,map-h);row.maximum=std::max(row.maximum,map-h);
              }
            }
          }
            });
          for(auto &thread:groundRows)thread.join();
          for(const auto &row:datumRows) {
            datumSum+=row.sum;datumCount+=row.count;
            datumMin=std::min(datumMin,row.minimum);datumMax=std::max(datumMax,row.maximum);
          }
          if(datumCount)
            std::cout<<"city ground datum (map - city) on the edge: mean "<<datumSum/double(datumCount)
                     <<" m, min "<<datumMin<<", max "<<datumMax<<" over "<<datumCount<<" samples\n";
          overlay.paintSpacingMillimetres=500;
          overlay.paintWidth=uint32_t((maxX-minX)/500);
          overlay.paintHeight=uint32_t((maxZ-minZ)/500);
          overlay.paint.resize(size_t(overlay.paintWidth)*overlay.paintHeight);
          // Each paint cell is independent; rows go to all cores.
          std::atomic<uint32_t> nextRow{0};
          std::vector<std::thread> painters;
          for(unsigned t=0;t<std::max(1u,std::thread::hardware_concurrency());++t)
            painters.emplace_back([&] {
          for(uint32_t z;(z=nextRow.fetch_add(1))<overlay.paintHeight;)
            for(uint32_t x=0;x<overlay.paintWidth;++x) {
              const auto w=cityGroundWeights(groundPaint,
                  double(minX+int64_t(x)*500+250)*.001,double(minZ+int64_t(z)*500+250)*.001);
              const uint32_t channel=w[0]>=w[1]&&w[0]>=w[2]?0u:w[1]>=w[2]?1u:2u;
              // Grass, packed earth, gravel.
              constexpr uint8_t paint[3]={3u,23u,24u};
              overlay.paint[size_t(z)*overlay.paintWidth+x]=paint[channel];
            }
            });
          for(auto &thread:painters)thread.join();
          for(const auto &prism:basePrisms) {
            voxel::svraster::CityGroundPrism out;
            const auto ringOf=[](const procedural::geom2::Ring &ring) {
              std::vector<std::array<int32_t,2>> points;
              for(const auto &p:ring)points.push_back({
                  int32_t(std::llround(procedural::geom2::toMetres(p.x)*1000.0)),
                  int32_t(std::llround(procedural::geom2::toMetres(p.y)*1000.0))});
              return points;
            };
            out.rings.push_back(ringOf(prism.footprint.outer));
            for(const auto &hole:prism.footprint.holes)out.rings.push_back(ringOf(hole));
            out.bottomMillimetres=int32_t(std::llround(prism.bottomMetres*1000.0));
            out.topMillimetres=int32_t(std::llround(prism.topMetres*1000.0));
            out.terrainFollowing=prism.terrainFollowing;
            // Sidewalks, treads and stair decks are flagstone; rock cuts and
            // courts are cut stone; every other paving is a cobbled road.
            const uint32_t m=prism.topMaterial;
            out.topMaterial=m==33u||m==18u||m==37u?22u:m==19u?25u:21u;
            out.sideMaterial=m==19u?25u:22u;
            if(out.rings[0].size()>=3)overlay.prisms.push_back(std::move(out));
          }
          if(!voxel::svraster::writeCityGroundOverlay((output/"city-ground.cgo").string(),overlay,error))
            return false;
          std::cout<<"city ground overlay: "<<overlay.groundWidth<<"x"<<overlay.groundHeight
                   <<" ground, "<<overlay.paintWidth<<"x"<<overlay.paintHeight<<" paint, "
                   <<overlay.prisms.size()<<" prisms, "
                   <<std::filesystem::file_size(output/"city-ground.cgo")<<" bytes, "
                   <<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()
                   <<" s\n";
        }
        if(exportScene) {
        if(!voxelNativeSampleLimit)
          for(int32_t tx=boundary.terrain.minTileX;tx<boundary.terrain.maxTileX;++tx)
            for(int32_t tz=boundary.terrain.minTileZ;tz<boundary.terrain.maxTileZ;++tz)
              nativeCity->terrainTiles.insert({tx,tz});
        std::map<std::pair<int32_t,int32_t>,std::vector<size_t>> baseByTile;
        for(size_t i=0;i<basePrisms.size();++i) {
          const auto box=procedural::geom2::bounds(basePrisms[i].footprint);
          constexpr double tileSize=32.0;
          const int32_t minX=int32_t(std::floor((procedural::geom2::toMetres(box.minX)-.1)/tileSize));
          const int32_t maxX=int32_t(std::floor((procedural::geom2::toMetres(box.maxX)+.1)/tileSize));
          const int32_t minZ=int32_t(std::floor((procedural::geom2::toMetres(box.minY)-.1)/tileSize));
          const int32_t maxZ=int32_t(std::floor((procedural::geom2::toMetres(box.maxY)+.1)/tileSize));
          for(int32_t tx=minX;tx<=maxX;++tx)for(int32_t tz=minZ;tz<=maxZ;++tz)
            if(nativeCity->terrainTiles.count({tx,tz}))baseByTile[{tx,tz}].push_back(i);
        }
        std::vector<std::pair<uint64_t,virtualgeometry::CityVoxelCityAssembler::Part>> terrainParts;
        std::unordered_set<uint64_t> usedPlacementIds;
        for(const auto &owner:nativeCity->owners)
          for(const auto &list:owner.parts)
            for(const auto &part:list)usedPlacementIds.insert(part.stableId);
        uint64_t nextId=0xf000000000000000ull;
        // With a ground overlay the renderer builds terrain, paving and
        // foundations on demand; the scene carries buildings only.
        if(!cityGroundOverlay)
        for(const auto &[tx,tz]:nativeCity->terrainTiles) {
          std::vector<virtualgeometry::CityVoxelBasePrism> tileBases;
          if(const auto found=baseByTile.find({tx,tz});found!=baseByTile.end()) {
            tileBases.reserve(found->second.size());
            for(const size_t index:found->second)tileBases.push_back(basePrisms[index]);
          }
          std::vector<procedural::geom2::Box> tileBaseBounds;
          tileBaseBounds.reserve(tileBases.size());
          for(const auto &prism:tileBases)
            tileBaseBounds.push_back(procedural::geom2::bounds(prism.footprint));
          virtualgeometry::CityVoxelTerrainTileSpec tile;
          tile.tileX=tx;tile.tileZ=tz;
          tile.cellsPerSide=CityVoxelTileGrid::voxelsPerTile;
          tile.overlapHaloCells=1;
          tile.naturalPalette=true;
          if(!tileBases.empty())tile.excludeColumn=[&tileBases,&tileBaseBounds](double x,double z) {
            const procedural::geom2::Point p{procedural::geom2::toUnit(x),
                procedural::geom2::toUnit(z)};
            for(size_t i=0;i<tileBases.size();++i)
              if(tileBaseBounds[i].contains(p)&&
                 procedural::geom2::contains(tileBases[i].footprint,p))return true;
            return false;
          };
          tile.topAppearance=[&groundPaint,ids=model.groundMaterialIds](double x,double z) {
            const auto weights=cityGroundWeights(groundPaint,x,z);
            const uint32_t xi=uint32_t(std::llround(x*10.0));
            const uint32_t zi=uint32_t(std::llround(z*10.0));
            uint32_t hash=xi*0x9e3779b1u ^ zi*0x85ebca6bu;
            hash^=hash>>16u;hash*=0x7feb352du;hash^=hash>>15u;
            const double selector=double(hash&0xffffu)/65536.0;
            const uint32_t channel=selector<weights[0]?0u:
                selector<weights[0]+weights[1]?1u:2u;
            const uint32_t variation=(hash>>24u)&7u;
            const uint32_t colors[3]={0xff3e6a48u,0xff866b53u,0xff8b9191u};
            return std::pair<uint32_t,uint32_t>{colors[channel]+variation,ids[channel]};
          };
          std::vector<voxel::svraster::CityVoxelCell> cells;
          if(!virtualgeometry::sampleCityVoxelTerrainTile(tile,ground,cells,error))return false;
          nativeCity->terrainCells+=cells.size();
          voxel::svraster::CityVoxelCompiledPlacement placement;
          while(voxelAudit->dwellingIds.count(nextId) || usedPlacementIds.count(nextId))++nextId;
          if(!nativeAssets->submit(nextId,scenePitchMicrometres,cells,placement,error))return false;
          virtualgeometry::CityVoxelCityAssembler::Part part{placement.stableId,
              placement.geometryId,placement.appearanceId,placement.originCell};
          terrainParts.push_back({nextId,part});
          ++nextId;
          if(!tileBases.empty()) {
            std::vector<voxel::svraster::CityVoxelCell> baseCells;
            if(!virtualgeometry::sampleCityVoxelBaseTile(tile,tileBases,baseCells,error,ground))return false;
            if(!baseCells.empty()) {
              nativeCity->baseCells+=baseCells.size();++nativeCity->baseTiles;
              while(voxelAudit->dwellingIds.count(nextId)||usedPlacementIds.count(nextId))++nextId;
              if(!nativeAssets->submit(nextId,scenePitchMicrometres,baseCells,placement,error))return false;
              terrainParts.push_back({nextId,{placement.stableId,placement.geometryId,
                  placement.appearanceId,placement.originCell}});
              ++nextId;
            }
          }
        }
        virtualgeometry::CityVoxelCityAssembler assembler(nativeAssets->geometry(),
            nativeAssets->appearances(),scenePitchMicrometres);
        // Turned frames need owner keys no building or terrain part uses.
        uint64_t frameKey=0xe000000000000000ull;
        for(const auto &owner:nativeCity->owners)
          for(uint8_t turn=0;turn<4;++turn) {
            if(owner.parts[turn].empty())continue;
            if(!assembler.addOwnerFrame(++frameKey,owner.originMillimetres,
                  uint16_t(owner.yawCode+turn*16384u),owner.parts[turn],error,owner.id))return false;
          }
        for(const auto &[id,part]:terrainParts)
          if(!assembler.addOwner(id,{0,0,0},0,{part},error))return false;
        virtualgeometry::CityVoxelStaticScene scene;
        std::unordered_map<uint64_t,uint64_t> ownerByPlacement;
        if(!assembler.finish(scene,ownerByPlacement,error))return false;
        std::set<uint32_t> profiles;
        for(const auto &appearance:nativeAssets->appearances()) {
          std::vector<voxel::svraster::CityVoxelCell> values;
          if(!voxel::svraster::decodeCityVoxelAppearance(appearance,values,error))return false;
          for(const auto &value:values)profiles.insert(value.materialProfile);
        }
        std::vector<voxel::svraster::CityVoxelMaterialReference> materials;
        for(uint32_t profile:profiles)
          materials.push_back({profile,0u,0x4349545900000000ull|uint64_t(profile+1)});
        if(!virtualgeometry::writeCityVoxelCompiledScene(
              (output/"native-city.cvscene").string(),scene,
              nativeAssets->geometry(),nativeAssets->appearances(),materials,
              nativeCity->owners.front().id,0x4349545900000001ull,{},error,&ownerByPlacement,
              {voxelTracePages,std::max(1u,std::thread::hardware_concurrency())}))return false;
        json manifest={{"pitchMetres",0.1},{"dwellingOwners",nativeCity->owners.size()},
            {"terrainTiles",nativeCity->terrainTiles.size()},
            {"terrainCells",nativeCity->terrainCells},
            {"baseTiles",nativeCity->baseTiles},{"baseCells",nativeCity->baseCells},
            {"sharedGeometry",nativeAssets->geometry().size()},
            {"appearances",nativeAssets->appearances().size()},
            {"placements",scene.placements().size()},
            {"dwellingBlocks",voxelAudit->sampledBlocks},
            {"geometryBytes",assembler.stats().encodedGeometryBytes},
            {"appearanceBytes",assembler.stats().appearanceBytes},
            {"placementBytes",assembler.stats().placementBytes}};
        manifest["owners"]=json::array();
        for(const auto &owner:nativeCity->owners)
          manifest["owners"].push_back({{"id",owner.id},
              {"originMillimetres",owner.originMillimetres},
              {"yawCode",owner.yawCode},{"parts",owner.partCount()}});
        if(!nativeQueue->farAssets.geometry().empty()) {
          virtualgeometry::CityVoxelCityAssembler farAssembler(nativeQueue->farAssets.geometry(),
              nativeQueue->farAssets.appearances(),nativeQueue->farPitch);
          for(const auto &owner:nativeCity->owners)
            if(owner.far.stableId &&
               !farAssembler.addOwnerFrame(owner.id,owner.originMillimetres,owner.yawCode,
                   {owner.far},error))return false;
          virtualgeometry::CityVoxelStaticScene farScene;
          std::unordered_map<uint64_t,uint64_t> farOwners;
          std::set<uint32_t> farProfiles;
          for(const auto &appearance:nativeQueue->farAssets.appearances()) {
            std::vector<voxel::svraster::CityVoxelCell> values;
            if(!voxel::svraster::decodeCityVoxelAppearance(appearance,values,error))return false;
            for(const auto &value:values)farProfiles.insert(value.materialProfile);
          }
          std::vector<voxel::svraster::CityVoxelMaterialReference> farMaterials;
          for(uint32_t profile:farProfiles)
            farMaterials.push_back({profile,0u,0x4349545900000000ull|uint64_t(profile+1)});
          if(!farAssembler.finish(farScene,farOwners,error) ||
             !virtualgeometry::writeCityVoxelCompiledScene(
                (output/"native-city-far.cvscene").string(),farScene,
                nativeQueue->farAssets.geometry(),nativeQueue->farAssets.appearances(),farMaterials,
                nativeCity->owners.front().id,0x4349545900000002ull,{},error,&farOwners,
                {voxelTracePages,std::max(1u,std::thread::hardware_concurrency())}))return false;
          const auto farStats=farAssembler.stats();
          manifest["far"]={{"pitchMicrometres",nativeQueue->farPitch},{"placements",farStats.placements},
              {"cells",nativeQueue->farCells},{"geometryBytes",farStats.encodedGeometryBytes},
              {"appearanceBytes",farStats.appearanceBytes}};
        }
        std::ofstream(output/"native-city.json")<<manifest.dump(2);
      }
        }
      voxelAudit->boundaryFeatures=boundary.features.size();
      voxelAudit->partySeams=boundary.partySeams.size();
      voxelAudit->partyContacts=boundary.partyContacts.size();
      for(const auto &seam:boundary.partySeams)voxelAudit->sharedPartySeams+=seam.shared;
      // Both incident tiles ask one immutable global height field for the
      // exact same border vertex. Check corners across the full city extent.
      for(int32_t x=boundary.terrain.minTileX;x<=boundary.terrain.maxTileX;++x)
        for(int32_t z=boundary.terrain.minTileZ;z<=boundary.terrain.maxTileZ;++z) {
          int64_t height=0;
          if(!cityVoxelTerrainSample(int64_t(x)*CityVoxelTileGrid::voxelsPerTile,
                                     int64_t(z)*CityVoxelTileGrid::voxelsPerTile,ground,height)) {
            if(error)*error="nonfinite terrain border sample"; return false;
          }
          ++voxelAudit->terrainBorderChecks;
        }
      return true;
    };
  }

  CityTerritory territory;
  bool composed = generateCityTerritory(site.brief.input, site.brief.suitability, site.brief.settings, site.brief.terrain, territory, &error);
  std::cout << "territory " << (composed ? "composed" : "rejected") << " " << error << "\n";
  report["territoryComposed"] = composed;
  report["territoryError"] = error;
  // Fabric and runtime through the class-based generator chain (per-tile jobs).
  SettlementFabric fabric;
  CitySkeleton citySkeleton;
  CityExteriorAllocation cityExterior;
  CityAgriculturePlan cityAgriculture;
  CityCastlePlan cityCastles;
  CityGrowthPlan cityGrowth;
  CityCompletionPlan cityComplete;
  CityReliefPlan cityRelief;
  CityModelPlan cityModel;
  RuntimeBuildResult built;
  bool fabricOK = false, skeletonOK = false, exteriorOK = false, agricultureOK = false, castlesOK = false, growthOK = false, completionOK = false, modelOK = false, runtimeOK = false;
  std::string fabricError, runtimeError;
  generator::RuntimeJobResults jobs;
  auto ledger = std::make_shared<generator::Ledger>();
  if (composed)
  {
    SettlementFabricSettings fabricSettings;
    fabricSettings.maximumTerraceStepMetres=terraceCutFill;
    fabricSettings.raisedDistricts=raisedDistricts;
    fabricSettings.variantSeed = variant;
    fabricSettings.recipeOverride = recipe;
    fabricSettings.patternOverride = pattern;
    fabricSettings.lotsOnly = lotsOnly || cityMeshSettings.infrastructureOnly;
    fabricSettings.roadSettlement = roads;
    fabricSettings.relief.amplitudeMetres = hills;
    fabricSettings.relief.terraceRiseMetres=terraceRise;
    fabricSettings.reliefPlan.stairStreets=terraceRise>0;
    fabricSettings.relief.wavelengthMetres = hillWavelength;
    fabricSettings.relief.seed = settings.brief.seed * 1000003ull + 17ull;
    fabricSettings.reliefPlan.maximumStreetGrade = streetGrade;
    SettlementRuntimeSettings runtimeSettings;
    runtimeSettings.domain = SettlementRuntimeDomain::Full;
    auto worldShared = std::make_shared<const GeneratedWorld>(world);
    WorldSiteGenerationBrief brief{worldShared, siteSettings};
    GenerationSnapshot chain;
    std::string chainError;
    bool ran = runGenerators(
        generator::settlementGeneratorTasks(1, ledger,cityMeshSettings),
        {{{"world.site-brief", 1}, {brief, 1}}, {{"settlement.fabric-settings", 1}, {fabricSettings, 1}}, {{"settlement.runtime-settings", 1}, {runtimeSettings, 1}}},
        settings.brief.seed,
        chain,
        {},
        &chainError);
    stage("generator chain");
    std::cout << "generator chain " << (ran ? "ran" : "failed: " + chainError) << " ledger rejected=" << ledger->count(generator::LedgerKind::Rejected) << " omitted=" << ledger->count(generator::LedgerKind::Omitted)
              << " capacity=" << ledger->count(generator::LedgerKind::Capacity) << " repaired=" << ledger->count(generator::LedgerKind::Repaired) << "\n";
    if (chain.count({"settlement.fabric", 1}))
    {
      fabric = std::any_cast<const SettlementFabric &>(chain.at({"settlement.fabric", 1}).value);
      fabricOK = true;
      std::cout << "fabric terrace=" << fabric.terraceAccepted << " layout=" << fabric.layoutAccepted << " streets=" << fabric.layout.streets.size() << " objects=" << fabric.layout.objects.size()
                << " housed=" << fabric.population.housed << " unhoused=" << fabric.population.unhoused << " buildings=" << fabric.population.buildings.size() << " compounds=" << fabric.compounds.size() << " districts=";
      for (const auto &d : fabric.layout.districts)
      {
        std::cout << districtUseName(d.use) << "(" << int(d.area) << ") ";
      }
      std::cout << "\n";
      for (auto &r : fabric.rejections)
      {
        std::cout << "  fabric: " << r << "\n";
      }
    }
    if (chain.count({"city.relief", 1}))
    {
      const auto &r = std::any_cast<const CityReliefPlan &>(chain.at({"city.relief", 1}).value);
      cityRelief=r;
      std::cout << "raised districts=" << r.platforms.size() << " reserved flights=" << r.stairs.size() << "\n";
      if (r.revision)
      {
        std::cout << "relief fall=" << r.fallMetres << " m levels=[" << r.minimumLevel << ".." << r.maximumLevel << "] terraced=" << r.terracedLots << " grounded=" << r.groundedLots
                  << " plinths=" << r.plinths << " maxPlinth=" << r.maximumPlinth << " m steepest=" << r.maximumGrade * 100. << " % steepPaved=" << r.steppedWays << " overBudget=" << r.overBudgetLots << " unserved=" << r.unservedLots << "\n";
      }
      else
      {
        std::cout << "relief none: the settlement is levelled onto one terrace\n";
      }
    }
    else
    {
      fabricError = chainError;
    }
    if (chain.count({"city.skeleton-complete", 1}))
    {
      citySkeleton = std::any_cast<const CitySkeleton &>(chain.at({"city.skeleton-complete", 1}).value);
      skeletonOK = true;
    }
    if (chain.count({"city.exterior", 1}))
    {
      cityExterior = std::any_cast<const CityExteriorAllocation &>(chain.at({"city.exterior", 1}).value);
      exteriorOK = true;
    }
    if (chain.count({"city.agriculture", 1}))
    {
      cityAgriculture = std::any_cast<const CityAgriculturePlan &>(chain.at({"city.agriculture", 1}).value);
      agricultureOK = true;
    }
    if (chain.count({"city.castles", 1}))
    {
      cityCastles = std::any_cast<const CityCastlePlan &>(chain.at({"city.castles", 1}).value);
      castlesOK = true;
    }
    if (chain.count({"city.growth", 1}))
    {
      cityGrowth = std::any_cast<const CityGrowthPlan &>(chain.at({"city.growth", 1}).value);
      growthOK = true;
    }
    if (chain.count({"city.complete", 1}))
    {
      cityComplete = std::any_cast<const CityCompletionPlan &>(chain.at({"city.complete", 1}).value);
      citySkeleton=cityComplete.skeleton; // Export the same reserved lots consumed by the native model.
      if (cityComplete.historicalGrowth) cityGrowth=*cityComplete.historicalGrowth;
      completionOK = true;
    }
    if (chain.count({"city.model", 1}))
    {
      cityModel = std::any_cast<const CityModelPlan &>(chain.at({"city.model", 1}).value);
      modelOK = true;
      std::cout << "city model buildings=" << cityModel.buildings.size() << " triangles=" << cityModel.mesh.triangles.size()
                << " roads=" << cityModel.roadTriangles << " lots=" << cityModel.lotTriangles << " compounds=" << cityModel.compoundTriangles << "\n";
    }
    if (chain.count({"runtime.results", 1}))
    {
      jobs = std::any_cast<const generator::RuntimeJobResults &>(chain.at({"runtime.results", 1}).value);
      size_t faces = 0, triangles = 0, structures = 0;
      for (auto &job : jobs.jobs)
      {
        if (!job.built)
        {
          continue;
        }
        faces += job.result.assets.levels[0].world.map.faces.size();
        triangles += job.result.assets.levels[0].world.mesh.triangles.size();
        structures += job.result.assets.structures.triangles.size();
      }
      runtimeOK = !jobs.jobs.empty() && jobs.built == jobs.jobs.size();
      if (!runtimeOK) runtimeError = "one or more runtime terrain tiles failed";
      std::cout << "runtime jobs=" << jobs.jobs.size() << " built=" << jobs.built << " faces=" << faces << " triangles=" << triangles << " structures=" << structures << "\n";
      for (auto &job : jobs.jobs)
      {
        if (!job.built)
        {
          std::cout << "  job " << job.name << ": " << job.error << "\n";
        }
      }
    }
    else
    {
      runtimeError = chainError;
    }
  }
  report["fabricComposed"] = fabricOK;
  report["fabricError"] = fabricError;
  report["runtimeBuilt"] = runtimeOK;
  report["runtimeError"] = runtimeError;
  const auto &w = site.brief.suitability;
  const double h = w.settings().spacingMetres;
  // Domain.
  {
    const double width = 1500, scale = width / site.bounds.maxX, height = site.bounds.maxZ * scale;
    Svg svg(width, height, scale);
    svg.rect(0, 0, site.bounds.maxX, site.bounds.maxZ, "#f4f1e8");
    std::vector<double> heights(w.cells().size());
    for (size_t i = 0; i < w.cells().size(); ++i)
    {
      const auto &c = w.cells()[i];
      heights[i] = c.center.y;
      std::string fill;
      if (c.reasons & Wet)
      {
        fill = "#7fb2e5";
      }
      else if (c.reasons & (Steep | Cliff | Brink))
      {
        fill = "#c49a6c";
      }
      else if (c.reasons & Rough)
      {
        fill = "#b8a48a";
      }
      else if (c.reasons & PoorSoil)
      {
        fill = "#e6dca0";
      }
      else if (c.reasons & Saturated)
      {
        fill = "#9fc7b8";
      }
      else if (c.reasons & Flood)
      {
        fill = "#a9c4e8";
      }
      else if (c.reasons & Inaccessible)
      {
        fill = "#d5d5d5";
      }
      else
      {
        continue;
      }
      svg.rect(c.center.x - h * .5, c.center.z - h * .5, h, h, fill, .8);
    }
    double lo = 1e300, hi = -1e300;
    for (auto v : heights)
    {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    double interval = std::max(1., std::round((hi - lo) / 12));
    for (double level = std::ceil(lo / interval) * interval; level <= hi; level += interval)
    {
      for (const auto &line : contourLines(heights, w.columns(), w.rows(), 0, 0, h, level))
      {
        svg.polyline(line, "#6b5a3a", std::fmod(level, interval * 5) == 0 ? 1 : .5, "stroke-opacity=\"0.6\"");
      }
    }
    for (const auto &r : site.regions)
    {
      // These are refined hinterland children, so their source biome—not the
      // coarse cover category—owns the visible land face.
      svg.polygon(r.boundary, biomeColour(Biome(r.biome)), "#6f675d", .35, .72);
    }
    // City planning children use the same subcell classification. Compatible
    // local variants and neighbour diffusion are assigned by refinement.
    for (const auto &cell : site.brief.input.cells)
    {
      svg.polygon(cell.boundary, biomeColour(Biome(cell.biome)), "#5b554d", .45, .72);
    }
    for (const auto &p : territory.rural.patches)
    {
      svg.polygon(p.boundary, "#e9d58a", "#9a7d1c", 1.2, .55);
    }
    for (const auto &f : territory.fields)
    {
      for (const auto &field : f.fields)
      {
        svg.polygon(field.boundary, "#d9b85c", "#7a5f10", .6, .85);
      }
    }
    for (const auto &c : territory.surfaces.contours)
    {
      if (c.role == SurfaceRole::Road)
      {
        svg.polygon(c.loop, "#b28b5a", "#6a4f2a", .3, .95);
      }
    }
    for (const auto &p : territory.rural.patches)
    {
      svg.polyline(p.access, "#7a5f10", 1.2, "stroke-dasharray=\"4 3\"");
    }
    for (const auto &hold : territory.holdings)
    {
      svg.polygon(hold.yard, "#a0703c", "#5a3a1a", .8, .9);
      for (const auto &b : hold.buildings)
      {
        svg.polygon(b.footprint, "#4a2e12", "#000", .4);
      }
    }
    for (const auto &cell : site.brief.input.cells)
    {
      // Hazards are already painted by the suitability grid above. Keep this
      // pass as topology only instead of replacing a child's biome with grey.
      svg.polygon(cell.boundary, "none", cell.buildable ? "#5b554d" : "#9b3f3f", .7, 1);
    }
    for (const auto &d : territory.regions.districts)
    {
      for (const auto &piece : d.pieces)
      {
        svg.polygon(piece, useColour(d.use), "#222", 1, .75);
      }
      svg.polyline(d.access, "#111", 1.5, "stroke-dasharray=\"6 4\"");
      if (!d.pieces.empty())
      {
        svg.text(centroid(d.pieces.front()), districtUseName(d.use), 11, "#000");
      }
    }
    if (fabricOK)
    {
      svg.polygon(fabric.polygon.outer, "#efe6c8", "#5a4a2a", 1.5, .9);
      for (const auto &c : fabric.layout.contours)
      {
        if (c.material == 1)
        {
          svg.polygon(c.loop, "#c9b28a", "none", 0, 1);
        }
      }
      for (const auto &o : fabric.layout.objects)
      {
        if (o.kind == CityObjectKind::House || o.kind == CityObjectKind::Workshop)
        {
          for (const auto &part : o.parts)
          {
            svg.polygon(part.footprint, "#8a5a3a", "#3a2a1a", .4, 1);
          }
        }
      }
      for (const auto &compound : fabric.compounds)
      {
        for (const auto &wall : compound.walls)
        {
          svg.polyline({wall.a, wall.b}, wall.gate ? "#f0d080" : "#202020", wall.thickness * 1.5 + .5);
        }
        for (const auto &tower : compound.towers)
        {
          svg.polygon(tower.footprint, "#404040", "#000", .4, 1);
        }
        for (const auto &wing : compound.wings)
        {
          svg.polygon(wing.brief.shell, wing.planned ? "#7a3a2a" : "#d04040", "#2a1a0a", .4, 1);
        }
      }
      for (const auto &a : fabric.arteries)
      {
        svg.polyline({a.a, a.b}, "#3a2a1a", 2.2);
      }
      for (const auto &approach : fabric.approaches)
      {
        svg.polyline(approach, "#6a4f2a", 2.6, "stroke-dasharray=\"8 5\"");
      }
      for (const auto &g : fabric.gates)
      {
        svg.circle(g.position, 5, "#ffffff", "#000", 1.5);
      }
    }
    for (const auto &job : jobs.jobs)
    {
      svg.polygon({{job.minX, 0, job.minZ}, {job.maxX, 0, job.minZ}, {job.maxX, 0, job.maxZ}, {job.minX, 0, job.maxZ}}, "none", job.built ? "#2a6f2a" : "#c0392b", 1.2, 1);
      svg.text({job.minX + 12, 0, job.maxZ - 22}, job.built ? "faces " + std::to_string(job.result.assets.levels[0].world.map.faces.size()) : "capacity", 12, job.built ? "#2a6f2a" : "#c0392b", "start");
    }
    for (const auto &g : site.brief.input.gateCells)
    {
      for (const auto &cell : site.brief.input.cells)
      {
        if (cell.id == g)
        {
          svg.circle(centroid(cell.boundary), 4, "#ffffff", "#444", 1);
        }
      }
    }
    for (const auto &src : site.brief.input.ruralSources)
    {
      svg.circle(src.exit, 6, "#ffcc00", "#000", 1.5);
    }
    svg.circle(site.localCentre, 8, "#e0342a", "#000", 1.5);
    svg.text(
        {20, 0, site.bounds.maxZ - 30},
        "site " + std::to_string(site.cell) + " (" + site.regionName + ")  contour " + std::to_string(int(interval)) + " m  population " + std::to_string(site.demand.population),
        16,
        "#000",
        "start");
    std::ofstream(output / "domain.svg") << svg.finish();
  }
  // JSON ledger.
  report["site"] = {
    {"id", site.id},
    {"cell", site.cell},
    {"region", site.regionName},
    {"centre", poly({site.centre})},
    {"origin", poly({site.origin})},
    {"size", {site.bounds.maxX, site.bounds.maxZ}},
    {"attempts", site.attempts},
    {"population", site.demand.population},
    {"households", site.households.size()},
    {"planningArea", site.planningArea},
    {"hinterlandArea", site.hinterlandArea},
    {"openArea", site.openArea}};
  for (const auto &c : site.candidates)
  {
    report["candidates"].push_back(
        {{"cell", c.cell},
         {"centre", poly({c.centre})},
         {"score", c.score},
         {"gradient", c.gradient},
         {"hinterlandGradient", c.hinterlandGradient},
         {"waterDistance", c.waterDistance},
         {"fertility", c.fertility},
         {"biome", biomeName(Biome(c.biome))},
         {"accepted", c.accepted},
         {"rejection", c.rejection}});
  }
  report["omissions"] = site.rejections;
  for (const auto &cell : site.brief.input.cells)
  {
    report["planningCells"].push_back(
        {{"id", cell.id},
         {"boundary", poly(cell.boundary)},
         {"slope", cell.slope},
         {"floodRisk", cell.floodRisk},
         {"fertility", cell.fertility},
         {"defense", cell.defense},
         {"buildable", cell.buildable},
         {"allocatable", cell.allocatable},
         {"navigableShore", cell.navigableShore}});
  }
  for (const auto &r : site.regions)
  {
    report["regions"].push_back({{"id", r.id}, {"boundary", poly(r.boundary)}, {"cover", int(r.cover)}});
  }
  for (const auto &src : site.brief.input.ruralSources)
  {
    report["ruralSources"].push_back({{"id", src.id}, {"exit", poly({src.exit})}, {"areaBudget", src.areaBudget}});
  }
  report["gateCells"] = site.brief.input.gateCells;
  for (const auto &d : site.brief.input.districts)
  {
    report["districtRequests"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"minimumArea", d.minimumArea}, {"targetArea", d.targetArea}});
  }
  report["suitability"] = {{"columns", w.columns()}, {"rows", w.rows()}, {"spacing", h}, {"fingerprint", w.fingerprint()}};
  {
    std::string reasons;
    for (const auto &c : w.cells())
    {
      reasons += c.reasons ? '1' : '0';
    }
    report["suitability"]["eligible"] = reasons;
  }
  if (composed)
  {
    report["territory"] = {
      {"urbanArea", territory.regions.assignedArea},
      {"cropArea", territory.cropArea},
      {"farmyardArea", territory.farmyardArea},
      {"patches", territory.rural.patches.size()},
      {"holdings", territory.holdings.size()},
      {"contours", territory.surfaces.contours.size()},
      {"eligibleArea", territory.rural.eligibleArea},
      {"patchArea", territory.rural.patchArea},
      {"unallocatedArea", territory.rural.unallocatedArea}};
    for (const auto &d : territory.regions.districts)
    {
      json pieces = json::array();
      for (auto q : d.pieces)
      {
        pieces.push_back(poly(q));
      }
      report["districts"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"cells", d.cells}, {"pieces", pieces}, {"access", poly(d.access)}, {"area", d.area}, {"shortfall", d.unmetTargetArea}});
    }
    for (const auto &o : territory.regions.omissions)
    {
      report["districtOmissions"].push_back({{"request", o.request}, {"reason", o.reason}});
    }
    for (size_t i = 0; i < territory.rural.patches.size(); ++i)
    {
      const auto &p = territory.rural.patches[i];
      json fields = json::array();
      for (const auto &f : territory.fields[i].fields)
      {
        fields.push_back({{"boundary", poly(f.boundary)}, {"area", f.area}});
      }
      report["patches"].push_back(
          {{"id", p.id},
           {"source", p.source},
           {"originalRegion", p.originalRegion},
           {"boundary", poly(p.boundary)},
           {"access", poly(p.access)},
           {"area", p.area},
           {"cropArea", territory.fields[i].cropArea},
           {"fields", fields}});
    }
    for (const auto &hold : territory.holdings)
    {
      json buildings = json::array();
      for (const auto &b : hold.buildings)
      {
        buildings.push_back({{"id", b.id}, {"use", int(b.use)}, {"footprint", poly(b.footprint)}, {"height", b.height}});
      }
      report["holdings"].push_back({{"patch", hold.patch}, {"yard", poly(hold.yard)}, {"buildings", buildings}});
    }
    report["ruralRejections"] = territory.rural.rejections;
    report["budgets"] = json::array();
    for (const auto &b : territory.rural.budgets)
    {
      report["budgets"].push_back({{"source", b.source}, {"requested", b.requested}, {"allocated", b.allocated}, {"shortfall", b.shortfall}});
    }
  }
  size_t voxelMissingCount=0;
  size_t voxelOverlapCount=0;
  size_t voxelModelMismatchCount=0;
  size_t voxelDuplicateBuildingIds=0;
  if(voxelAssemblyAudit) {
    nativeQueue->drain();
    if(!nativeQueue->failure().empty())
      std::cerr<<"native voxel compile failed: "<<nativeQueue->failure()<<"\n";
    json missing=json::array();
    json modelBuildingOwners=json::array();
    std::unordered_set<uint64_t> modelIds;
    if(modelOK)for(const auto &building:cityModel.buildings) {
      if(!modelIds.insert(building.id).second)++voxelDuplicateBuildingIds;
      if(!voxelAudit->covered.count(building.id))missing.push_back(building.id);
      modelBuildingOwners.push_back({{"id",building.id},
          {"category",cityModelBuildingKindName(building.kind)}});
    }
    std::sort(modelBuildingOwners.begin(),modelBuildingOwners.end(),
        [](const json &a,const json &b){return a.at("id").get<uint64_t>()<b.at("id").get<uint64_t>();});
    if(modelOK)for(const auto &building:cityModel.buildings) {
      const auto source=voxelAudit->sourceCounts.find(building.id);
      if(source==voxelAudit->sourceCounts.end())continue;
      const auto &counts=source->second;
      if(counts.pieces!=building.pieces || counts.rooms!=building.rooms ||
         counts.roomConnections!=building.roomConnections ||
         counts.windows!=building.windows ||
         std::abs(counts.base-building.base)>1e-6)++voxelModelMismatchCount;
      for(const auto &point:building.footprint)
        if(point.x<counts.minX-.01 || point.x>counts.maxX+.01 ||
           point.z<counts.minZ-.01 || point.z>counts.maxZ+.01)
          {++voxelModelMismatchCount;break;}
    }
    voxelMissingCount=missing.size();
    std::sort(voxelAudit->triangleSpans.begin(),voxelAudit->triangleSpans.end());
    for(size_t i=1;i<voxelAudit->triangleSpans.size();++i)
      if(voxelAudit->triangleSpans[i].first<voxelAudit->triangleSpans[i-1].second)
        ++voxelOverlapCount;
    report["voxelAssemblyAudit"]={{"dwellings",voxelAudit->dwellings},
      {"parts",voxelAudit->parts},{"sourceFaces",voxelAudit->faces},
      {"facades",voxelAudit->facades},{"connectors",voxelAudit->connectors},
      {"legacyTriangles",voxelAudit->legacyTriangles},
      {"legacyPanels",voxelAudit->legacyPanels},
      {"unalignedPlacements",voxelAudit->unaligned},
      {"uniqueOrientations",voxelAudit->uniqueOrientations},
      {"bakedPlacements",voxelAudit->bakedPlacements},
      {"partyParts",voxelAudit->partyParts},
      {"foundationJoins",voxelAudit->foundationJoins},
      {"foundationSamples",voxelAudit->foundationSamples},
      {"boundaryFeatures",voxelAudit->boundaryFeatures},
      {"partySeams",voxelAudit->partySeams},
      {"sharedPartySeams",voxelAudit->sharedPartySeams},
      {"partyContacts",voxelAudit->partyContacts},
      {"terrainBorderChecks",voxelAudit->terrainBorderChecks},
      {"uniqueMeshes",voxelAudit->uniqueMeshes},
      {"uniqueTriangles",voxelAudit->uniqueTriangles},
      {"sampledDwellings",voxelAudit->sampledDwellings},
      {"sampledParts",voxelAudit->sampledParts},
      {"sampledBlocks",voxelAudit->sampledBlocks},
      {"sampledHouseShapes",nativeQueue->houseShapes.size()},
      {"civilOwners",nativeQueue->civilOwners},{"civilCells",nativeQueue->civilCells},
      {"failedHouses",[&]{
        json failed=json::array();
        for(const auto &[id,why]:nativeQueue->failedHouses)failed.push_back({{"id",id},{"error",why}});
        return failed;}()},
      {"sampledFarCells",voxelAudit->sampledFarCells},
      {"sampledFarGeometryBytes",[&]{uint64_t b=0;for(const auto &g:nativeQueue->farAssets.geometry())b+=g.bytes.size();return b;}()},
      {"sampledHouseCells",nativeQueue->houseCells},
      {"blockSweep",[&]{
        json sweep=json::array();
        for(const auto &entry:nativeQueue->sweep) {
          uint64_t g=0,a=0;
          for(const auto &asset:entry.assets.geometry())g+=asset.bytes.size();
          for(const auto &asset:entry.assets.appearances())a+=asset.bytes.size();
          sweep.push_back({{"across",entry.settings.horizontalVoxels},{"up",entry.settings.verticalVoxels},
              {"blocks",entry.blocks},{"geometries",entry.assets.geometry().size()},
              {"appearances",entry.assets.appearances().size()},
              {"geometryBytes",g},{"appearanceBytes",a},{"placementBytes",entry.blocks*32}});
        }
        return sweep;}()},
      {"sampledGeometryAssets",nativeAssets->geometry().size()},
      {"sampledAppearanceAssets",nativeAssets->appearances().size()},
      {"sampledGeometryBytes",[&]{uint64_t b=0;for(const auto &g:nativeAssets->geometry())b+=g.bytes.size();return b;}()},
      {"sampledAppearanceBytes",[&]{uint64_t b=0;for(const auto &a:nativeAssets->appearances())b+=a.bytes.size();return b;}()},
      {"sampledCells",voxelAudit->sampledCells},
      {"sampledClipped",voxelAudit->sampledClipped},
      {"sampledCoveredStaticParts",voxelAudit->sampledCoveredStaticParts},
      {"sampledPairedFacadeSides",voxelAudit->sampledPairedFacadeSides},
      {"sampledDecorativeOverlays",voxelAudit->sampledDecorativeOverlays},
      {"sampledOverlayCells",voxelAudit->sampledOverlayCells},
      {"sampledGeometries",nativeAssets->report().distinctGeometry},
      {"sampledAppearances",nativeAssets->report().distinctAppearances},
      {"sampledGeometryReuses",nativeAssets->report().geometryReuses},
      {"sampledGeometryBytes",nativeAssets->report().geometryBytes},
      {"sampledAppearanceBytes",nativeAssets->report().appearanceBytes},
      {"sampledPeakScratchBytes",nativeAssets->report().peakScratchBytes},
      {"sampledTerrainTiles",voxelAudit->sampledTerrainTiles},
      {"sampledTerrainCells",voxelAudit->sampledTerrainCells},
      {"geometryKeys",voxelAudit->geometryKeys.size()},
      {"appearanceKeys",voxelAudit->appearanceKeys.size()},
      {"overlappingTriangleSpans",voxelOverlapCount},
      {"modelMismatches",voxelModelMismatchCount},
      {"duplicateBuildingIds",voxelDuplicateBuildingIds},
      {"coveredBuildings",voxelAudit->covered.size()},
      {"modelBuildings",modelOK?cityModel.buildings.size():0},
      {"modelBuildingOwners",modelBuildingOwners},
      {"missingBuildingIds",missing}};
  }
  std::ofstream(output / "settlement.json") << report.dump(2);
  // Existing Python renderer schemas: city-territory.json (render_city_territory.territory)
  // and village-layout.json (render_city_layout_comparison.panel).
  if (composed)
  {
    json t;
    t["revision"] = territory.revision;
    t["cropArea"] = territory.cropArea;
    t["farmyardArea"] = territory.farmyardArea;
    t["rejections"] = territory.rural.rejections;
    t["bounds"] = {0, 0, site.bounds.maxX, site.bounds.maxZ};
    t["regions"] = json::array();
    for (const auto &d : territory.regions.districts)
    {
      json pieces = json::array();
      for (auto q : d.pieces)
      {
        pieces.push_back(poly(q));
      }
      t["regions"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"pieces", pieces}, {"access", poly(d.access)}, {"area", d.area}, {"shortfall", d.unmetTargetArea}});
    }
    t["patches"] = json::array();
    for (size_t i = 0; i < territory.rural.patches.size(); ++i)
    {
      auto &r = territory.rural.patches[i];
      json fields = json::array();
      for (auto &f : territory.fields[i].fields)
      {
        fields.push_back({{"boundary", poly(f.boundary)}, {"access", poly(f.access)}, {"area", f.area}});
      }
      t["patches"].push_back(
          {{"id", r.id},
           {"source", r.source},
           {"originalRegion", r.originalRegion},
           {"boundary", poly(r.boundary)},
           {"access", poly(r.access)},
           {"area", r.area},
           {"fields", fields},
           {"cropArea", territory.fields[i].cropArea}});
    }
    t["holdings"] = json::array();
    for (auto &hold : territory.holdings)
    {
      json buildings = json::array();
      for (auto &b : hold.buildings)
      {
        buildings.push_back({{"id", b.id}, {"use", int(b.use)}, {"footprint", poly(b.footprint)}, {"entrance", poly({b.entrance})}, {"height", b.height}});
      }
      t["holdings"].push_back({{"patch", hold.patch}, {"source", hold.source}, {"yard", poly(hold.yard)}, {"access", poly(hold.access)}, {"buildings", buildings}});
    }
    t["contours"] = json::array();
    for (auto c : territory.surfaces.contours)
    {
      t["contours"].push_back({{"id", c.id}, {"material", c.material}, {"role", int(c.role)}, {"loop", poly(c.loop)}});
    }
    t["originalRegions"] = json::array();
    for (auto &r : site.regions)
    {
      t["originalRegions"].push_back({{"id", r.id}, {"boundary", poly(r.boundary)}, {"cover", int(r.cover)}, {"reserved", r.reserved}});
    }
    t["occupied"] = json::array();
    t["suitabilitySource"] = "actual generated world: map surface elevation, explicit ocean/lake/river water, climate moisture, biome fertility class";
    auto encodeLayout = [&](const CityLayout &c) {
      json j;
      j["revision"] = c.revision;
      j["envelope"] = poly(c.envelope.outer);
      j["acceptedEnvelope"] = poly(c.envelope.outer);
      j["holes"] = json::array();
      for (auto &hole : c.envelope.holes)
      {
        j["holes"].push_back(poly(hole));
      }
      j["suburbs"] = json::array();
      for (auto &p : c.suburbs)
      {
        j["suburbs"].push_back(poly(p));
      }
      j["reservations"] = json::array();
      for (auto &p : c.reservations)
      {
        j["reservations"].push_back(poly(p));
      }
      j["towers"] = json::array();
      for (auto &tw : c.towers)
      {
        j["towers"].push_back({{"id", tw.id}, {"footprint", poly(tw.footprint)}, {"height", tw.height}});
      }
      j["districts"] = json::array();
      for (auto &d : c.districts)
      {
        json pieces = json::array();
        for (auto &p : d.pieces)
        {
          pieces.push_back(poly(p));
        }
        j["districts"].push_back({{"id", d.id}, {"use", int(d.use)}, {"name", districtUseName(d.use)}, {"pieces", pieces}, {"area", d.area}, {"access", poly(d.access)}});
      }
      j["wards"] = json::array();
      for (auto &ward : c.wards)
      {
        json pieces = json::array();
        for (auto &p : ward.pieces)
        {
          pieces.push_back(poly(p));
        }
        j["wards"].push_back({{"id", ward.id}, {"pieces", pieces}});
      }
      j["streets"] = json::array();
      for (auto &st : c.streets)
      {
        j["streets"].push_back(
            {{"id", st.id}, {"a", {st.a.x, st.a.y, st.a.z}}, {"b", {st.b.x, st.b.y, st.b.z}}, {"width", st.halfWidth * 2}, {"arterial", st.arterial}, {"hierarchy", st.hierarchy}, {"path", poly(st.path)}});
      }
      j["objects"] = json::array();
      for (auto &o : c.objects)
      {
        json parts = json::array();
        for (auto &part : o.parts)
        {
          parts.push_back({{"footprint", poly(part.footprint)}, {"height", part.height}});
        }
        j["objects"].push_back(
            {{"id", o.id},
             {"ward", o.ward},
             {"block", o.block},
             {"kind", int(o.kind)},
             {"footprint", poly(o.footprint)},
             {"clearance", poly(o.clearance)},
             {"entrance", {o.entrance.x, o.entrance.y, o.entrance.z}},
             {"access", {o.access.x, o.access.y, o.access.z}},
             {"foundation", o.foundation},
             {"height", o.height},
             {"buildingType", int(o.buildingType)},
             {"parts", parts},
             {"courtyard", poly(o.courtyard)},
             {"compactFallback", o.compactFallback}});
      }
      j["blocks"] = json::array();
      for (auto &b : c.blocks)
      {
        json courts = json::array();
        for (auto &p : b.courtyards)
        {
          courts.push_back(poly(p.outer));
        }
        json lots = json::array(), openLots = json::array();
        for (auto &p : b.lots)
        {
          lots.push_back(poly(p));
        }
        for (auto &p : b.openLots)
        {
          openLots.push_back(poly(p));
        }
        j["blocks"].push_back({{"id", b.id}, {"courtyards", courts}, {"use", int(b.use)}, {"district", b.district}, {"rejection", b.rejection}, {"outer", poly(b.boundary.outer)}, {"lots", lots}, {"openLots", openLots}});
      }
      j["walls"] = json::array();
      j["traces"] = json::array();
      for (auto &wall : c.walls)
      {
        j["walls"].push_back({poly({wall.a, wall.b}), wall.halfWidth});
      }
      j["contours"] = json::array();
      for (auto &c2 : c.contours)
      {
        j["contours"].push_back({{"id", c2.id}, {"material", c2.material}, {"role", int(c2.role)}, {"loop", poly(c2.loop)}});
      }
      j["subdivisionCells"] = c.subdivisionCells;
      j["rejected"] = c.rejectedAreas;
      return j;
    };
    if (fabricOK && fabric.layoutAccepted)
    {
      auto layout = encodeLayout(fabric.layout);
      t["city"] = layout;
      layout["terraceLevel"] = fabric.terraceLevel;
      layout["terraceStep"] = fabric.terraceStep;
      layout["housed"] = fabric.population.housed;
      layout["unhoused"] = fabric.population.unhoused;
      layout["buildings"] = fabric.population.buildings.size();
      layout["rejections"] = fabric.rejections;
      std::ofstream(output / "village-layout.json") << layout.dump(2);
      // Fitted interiors in the v9 director schema, for render_settlement_director.floor_panel.
      size_t exported = 0;
      std::vector<const SettlementBuilding *> ordered;
      for (const auto &building : fabric.population.buildings)
      {
        ordered.push_back(&building);
      }
      std::stable_sort(ordered.begin(), ordered.end(), [](const SettlementBuilding *a, const SettlementBuilding *b) {
        if (a->interior.floors.size() != b->interior.floors.size())
        {
          return a->interior.floors.size() > b->interior.floors.size();
        }
        if (a->programme != b->programme)
        {
          return a->programme < b->programme;
        }
        return a->interior.rooms.size() > b->interior.rooms.size();
      });
      std::map<std::string, int> perProgramme;
      for (const auto *buildingPointer : ordered)
      {
        const auto &building = *buildingPointer;
        const auto &d = building.interior;
        if (d.rooms.empty() || exported >= 8 || perProgramme[building.programme]++ >= 3)
        {
          continue;
        }
        json j = {{"id", d.id}, {"use", int(d.use)}, {"residents", d.residentCapacity}, {"workers", d.workerCapacity}, {"programme", building.programme}, {"fallback", d.fallback}, {"note", d.note}};
        j["rooms"] = json::array();
        j["walls"] = json::array();
        j["doors"] = json::array();
        j["floors"] = json::array();
        j["stairs"] = json::array();
        for (auto &r : d.rooms)
        {
          j["rooms"].push_back({{"id", r.id}, {"floor", r.floor}, {"use", int(r.use)}, {"boundary", poly(r.boundary)}, {"area", r.usableArea}});
        }
        for (auto &wl : d.walls)
        {
          j["walls"].push_back({{"id", wl.id}, {"floor", wl.floor}, {"path", poly({wl.a, wl.b})}, {"thickness", wl.thickness}, {"exterior", wl.exterior}});
        }
        for (auto &dr : d.doors)
        {
          j["doors"].push_back(
              {{"wall", dr.wall}, {"room", dr.room}, {"floor", dr.floor}, {"center", poly({dr.center})}, {"width", dr.width}, {"private", dr.privateAccess}, {"exterior", dr.exterior}, {"shopOpening", dr.shopOpening}});
        }
        for (auto &f : d.floors)
        {
          json c = json::array(), sl = json::array();
          for (auto &q : f.circulation)
          {
            c.push_back(poly(q));
          }
          for (auto &q : f.slabs)
          {
            sl.push_back(poly(q));
          }
          j["floors"].push_back(
              {{"elevation", f.elevation},
               {"height", f.height},
               {"gross", f.grossArea},
               {"roomArea", f.roomArea},
               {"circulationArea", f.circulationArea},
               {"wallArea", f.wallArea},
               {"coreArea", f.coreArea},
               {"circulation", c},
               {"slabs", sl}});
        }
        for (auto &st : d.stairs)
        {
          j["stairs"].push_back(
              {{"opening", poly(st.opening)}, {"lower", poly(st.lowerLanding)}, {"upper", poly(st.upperLanding)}, {"risers", st.risers}, {"rise", st.rise}, {"tread", st.tread}, {"private", st.privateAccess}});
        }
        std::ofstream(output / ("dwelling-" + std::to_string(exported++) + ".json")) << j.dump(2);
      }
    }
    std::ofstream(output / "city-territory.json") << t.dump(2);
    if (skeletonOK)
    {
      json j;
      j["regions"] = json::array();
      j["contacts"] = json::array();
      j["roads"] = json::array();
      j["routes"] = json::array();
      j["roadNodes"] = json::array();
      j["walls"] = json::array();
      j["gates"] = json::array();
      for (const auto &r : citySkeleton.regions)
        j["regions"].push_back({{"id", r.id}, {"parent", r.parent}, {"kind", cityRegionKindName(r.kind)}, {"use", cityLandUseName(r.landUse)}, {"boundary", poly(r.boundary)}});
      for (const auto &c : citySkeleton.regionContacts)
        j["contacts"].push_back({{"id", c.id}, {"a", c.a}, {"b", c.b}, {"kind", int(c.kind)}, {"point", {c.point.x, c.point.y, c.point.z}}, {"length", c.sharedLength}});
      for (const auto &e : citySkeleton.roadEdges)
        j["roads"].push_back({{"id", e.id}, {"from", e.from}, {"to", e.to}, {"kind", int(e.kind)}, {"width", e.halfWidth}, {"path", poly(e.path)}});
      for (const auto &n : citySkeleton.roadNodes)
        j["roadNodes"].push_back({{"id", n.id}, {"kind", int(n.kind)}, {"position", {n.position.x, n.position.y, n.position.z}}, {"degree", n.edges.size()}});
      for (const auto &e : citySkeleton.wallEdges)
        j["walls"].push_back({{"id", e.id}, {"from", e.from}, {"to", e.to}, {"a", {e.a.x, e.a.y, e.a.z}}, {"b", {e.b.x, e.b.y, e.b.z}}});
      for (const auto &g : citySkeleton.gates)
        j["gates"].push_back({{"id", g.id}, {"wallNode", g.wallNode}, {"roadNode", g.roadNode}, {"position", {g.position.x, g.position.y, g.position.z}}});
      std::ofstream(output / "city-skeleton-c1.json") << j.dump(2);
    }
    if (exteriorOK)
    {
      json j;
      j["parcels"] = json::array();
      j["roads"] = json::array();
      for (const auto &p : cityExterior.parcels)
        j["parcels"].push_back({{"id", p.id}, {"cell", p.cell}, {"sourceNode", p.sourceNode}, {"parentNode", p.parentNode}, {"mapRegion", p.mapRegion}, {"biome", p.biome}, {"use", cityLandUseName(p.use)}, {"boundary", poly(p.boundary)}, {"reason", p.reason}, {"access", p.accessNode}});
      for (const auto &road : citySkeleton.roadEdges)
        j["roads"].push_back({{"path", poly(road.path)}, {"width", road.halfWidth}});
      std::ofstream(output / "city-exterior-c2.json") << j.dump(2);
    }
    if (agricultureOK && composed)
    {
      std::map<uint64_t, const CityAgriculturalParcel *> selected;
      for (const auto &p : cityAgriculture.parcels)
        selected[p.parcel] = &p;
      json j;
      j["required"] = cityAgriculture.requiredAnnualYield;
      j["usable"] = cityAgriculture.annualUsableYield;
      j["parcels"] = json::array();
      for (const auto &product : territory.fields)
        for (const auto &p : product.fields)
          j["parcels"].push_back({{"id", p.id}, {"use", "arable"}, {"boundary", poly(p.boundary)}, {"selected", selected.count(p.id) != 0}, {"holding", selected.count(p.id) ? selected[p.id]->holding : 0}});
      std::ofstream(output / "city-agriculture-c3.json") << j.dump(2);
    }
    if (growthOK)
    {
      json j;
      j["walls"] = json::array();
      j["zones"] = json::array();
      j["blocks"] = json::array();
      j["lots"] = json::array();
      j["roads"] = json::array();
      j["routes"] = json::array();
      j["historical"]=cityGrowth.historical;
      j["omissions"]=cityGrowth.omissions;
      j["epochs"]=json::array();
      for(const auto &epoch:cityGrowth.epochs)
        j["epochs"].push_back({{"builtWeek",epoch.builtWeek},{"buildings",epoch.buildings},{"occupiedArea",epoch.occupiedArea},{"blocks",epoch.blocks}});
      for (const auto &wall : cityGrowth.walls)
        j["walls"].push_back({{"id", wall.id}, {"era", wall.era}, {"lifecycle", cityWallLifecycleName(wall.lifecycle)}, {"boundary", poly(wall.boundary)}, {"gates", poly(wall.gates)}, {"towers", poly(wall.towers)}, {"area", wall.enclosedArea}});
      for (const auto &trace : cityGrowth.traces)
        j["traces"].push_back({{"id", trace.id}, {"era", trace.era}, {"lifecycle", cityWallLifecycleName(trace.lifecycle)}, {"boundary", poly(trace.boundary)}, {"area", trace.enclosedArea}});
      for (const auto &zone : cityGrowth.zones)
        j["zones"].push_back({{"id", zone.id}, {"source", zone.source}, {"kind", cityGrowthZoneKindName(zone.kind)}, {"boundary", poly(zone.boundary)}, {"insideStanding", zone.insideStandingWall}, {"insidePlanned", zone.insidePlannedWall}});
      for (const auto &region : citySkeleton.regions)
      {
        if (region.kind == CityRegionKind::Block)
          j["blocks"].push_back({{"id", region.id}, {"boundary", poly(region.boundary)}, {"exterior", region.exterior}});
        else if (region.kind == CityRegionKind::Lot)
          j["lots"].push_back({{"id", region.id}, {"parent", region.parent}, {"boundary", poly(region.boundary)}, {"exterior", region.exterior}});
      }
      for (const auto &road : citySkeleton.roadEdges)
        if (road.kind != CityRoadKind::FieldTrack)
          j["roads"].push_back({{"kind", int(road.kind)}, {"width", road.halfWidth * 2}, {"path", poly(road.path)}});
      j["castlesComposed"] = castlesOK;
      std::ofstream(output / "city-growth-c5.json") << j.dump(2);
    }
    if (completionOK)
    {
      json j;
      j["wallSegments"] = json::array();
      j["gatehouses"] = json::array();
      j["towers"] = json::array();
      j["suburbs"] = json::array();
      j["harbours"] = json::array();
      j["history"] = json::array();
      j["growthRegions"] = json::array();
      j["blocks"] = json::array();
      j["lots"] = json::array();
      j["roads"] = json::array();
      j["routes"] = json::array();
      for (const auto &segment : cityComplete.wallSegments)
      {
        json faces = json::array();
        for (const auto &face : segment.faces)
          faces.push_back({{"kind", cityWallFaceKindName(face.kind)}, {"corners", poly(std::vector<Position>(face.corners.begin(), face.corners.end()))}});
        j["wallSegments"].push_back({{"id", segment.id}, {"wall", segment.wall}, {"era", segment.era}, {"lifecycle", cityWallLifecycleName(segment.lifecycle)},
                                      {"a", poly({segment.a})[0]}, {"b", poly({segment.b})[0]}, {"width", segment.width}, {"height", segment.height}, {"depth", segment.depth},
                                      {"built", segment.built}, {"wallWalk", segment.wallWalkContinuous}, {"sharedCastle", segment.sharedWithCastle}, {"faces", faces}});
      }
      for (const auto &gate : cityComplete.gatehouses)
        j["gatehouses"].push_back({{"id", gate.id}, {"wall", gate.wall}, {"era", gate.era}, {"position", poly({gate.position})[0]}, {"openingWidth", gate.openingWidth},
                                    {"depth", gate.depth}, {"height", gate.height}, {"wallNode", gate.wallNode}, {"roadNode", gate.roadNode}, {"built", gate.built}});
      for (const auto &tower : cityComplete.towers)
        j["towers"].push_back({{"id", tower.id}, {"wall", tower.wall}, {"era", tower.era}, {"lifecycle", cityWallLifecycleName(tower.lifecycle)}, {"position", poly({tower.position})[0]},
                               {"radius", tower.radius}, {"height", tower.height}, {"built", tower.built}});
      for (const auto &suburb : cityComplete.suburbs)
        j["suburbs"].push_back({{"id", suburb.id}, {"zone", suburb.zone}, {"accessNode", suburb.accessNode}, {"boundary", poly(suburb.boundary)}, {"gateDistance", suburb.gateDistance}, {"density", suburb.density}});
      for (const auto &harbour : cityComplete.harbours)
        j["harbours"].push_back({{"id", harbour.id}, {"region", harbour.region}, {"accessNode", harbour.accessNode}, {"castle", harbour.controllingCastle}, {"quay", poly(harbour.quay)}});
      for (const auto &history : cityComplete.history)
        j["history"].push_back({{"older", history.olderWall}, {"newer", history.newerWall}, {"olderState", cityWallLifecycleName(history.olderState)}});
      for (const auto &trace : cityGrowth.traces)
        j["growthRegions"].push_back({{"id", trace.id}, {"kind", cityWallLifecycleName(trace.lifecycle)}, {"boundary", poly(trace.boundary)}});
      for (const auto &region : cityComplete.skeleton.regions)
      {
        if (region.kind == CityRegionKind::Block)
          j["blocks"].push_back({{"id", region.id}, {"boundary", poly(region.boundary)}});
        else if (region.kind == CityRegionKind::Lot)
        {
          const auto state = std::find_if(cityComplete.lots.begin(), cityComplete.lots.end(), [&](const auto &lot) { return lot.lot == region.id; });
          j["lots"].push_back({{"id", region.id}, {"parent", region.parent}, {"boundary", poly(region.boundary)},
                                {"developed", state == cityComplete.lots.end() || state->developed}, {"density", state == cityComplete.lots.end() ? 1 : state->density}});
          if (state != cityComplete.lots.end())
            j["lots"].back()["allocation"]={{"building",state->merchantHouse ? "shop-house" : "family-house"},
              {"preferredFootprintMetres2",state->preferredFootprintMetres2},{"desiredRooms",state->desiredRooms},
              {"preferredRoomMetres2",state->preferredRoomMetres2},{"maximumFloors",state->maximumFloors},{"minimumFloors",state->minimumFloors},
              {"rowHouse",state->rowHouse},{"residence",int(state->residence)}};
        }
      }
      for (const auto &road : cityComplete.skeleton.roadEdges)
        j["roads"].push_back({{"id", road.id}, {"kind", int(road.kind)}, {"width", road.halfWidth * 2}, {"path", poly(road.path)}});
      for (const auto &route : cityComplete.routes)
        j["routes"].push_back({{"id", route.id}, {"kind", int(route.kind)}, {"edges", route.edges}, {"width", route.halfWidth * 2}, {"path", poly(route.path)}});
      j["graph"] = {{"wallNodes", cityComplete.skeleton.wallNodes.size()}, {"wallEdges", cityComplete.skeleton.wallEdges.size()}, {"roadNodes", cityComplete.skeleton.roadNodes.size()},
                      {"roadEdges", cityComplete.skeleton.roadEdges.size()}, {"gates", cityComplete.skeleton.gates.size()}};
      std::map<std::string, size_t> exteriorUses;
      for (const auto &parcel : cityComplete.exterior.parcels) ++exteriorUses[cityLandUseName(parcel.use)];
      j["exteriorUses"] = exteriorUses;
      j["historicalBindings"]=json::array();
      for(const auto &binding:cityComplete.historicalLots)
        j["historicalBindings"].push_back({{"historicalBuilding",binding.building},{"lot",binding.lot}});
      std::ofstream(output / "city-complete-c9.json") << j.dump(2);
    }
    if (modelOK && !voxelAuditOnly)
    {
      json j;

      // The mesh goes out as bytes, not as text.
      //
      // Eleven million triangles with their corners, UVs, materials and
      // ground weights spelled out in JSON was a 2.3 GB file, and building
      // and writing it took 143 s - a quarter of the whole run - before the
      // viewer read all of it back in Python. The arrays go in a side-car
      // binary with their offsets recorded here; everything a human or a
      // script reads stays in the JSON.
      json arrays;
      {
        std::ofstream binary(output / "city-model-c10.bin", std::ios::binary);
        uint64_t at = 0;
        const auto put = [&](const char *name, const void *data, const size_t bytes, const char *type, const json &shape) {
          binary.write(static_cast<const char *>(data), std::streamsize(bytes));
          arrays[name] = {{"offset", at}, {"bytes", bytes}, {"type", type}, {"shape", shape}};
          at += bytes;
        };

        std::vector<double> vertices;
        vertices.reserve(cityModel.mesh.vertices.size() * 3);
        for (const auto &v : cityModel.mesh.vertices) { vertices.push_back(v.x); vertices.push_back(v.y); vertices.push_back(v.z); }
        put("vertices", vertices.data(), vertices.size() * sizeof(double), "f8", json::array({3}));

        std::vector<uint32_t> triangles;
        triangles.reserve(cityModel.mesh.triangles.size() * 3);
        for (const auto &t : cityModel.mesh.triangles) { triangles.push_back(t[0]); triangles.push_back(t[1]); triangles.push_back(t[2]); }
        put("triangles", triangles.data(), triangles.size() * sizeof(uint32_t), "u4", json::array({3}));

        put("materials", cityModel.materials.data(), cityModel.materials.size() * sizeof(uint32_t), "u4", json::array());
        put("surfaceMaterials", cityModel.surfaceMaterials.data(), cityModel.surfaceMaterials.size() * sizeof(uint32_t), "u4", json::array());

        std::vector<float> uvs;
        uvs.reserve(cityModel.triangleUVs.size() * 6);
        for (const auto &corner : cityModel.triangleUVs)
          for (const auto &uv : corner) { uvs.push_back(float(uv[0])); uvs.push_back(float(uv[1])); }
        put("triangleUVs", uvs.data(), uvs.size() * sizeof(float), "f4", json::array({3, 2}));

        std::vector<float> weights;
        weights.reserve(cityModel.groundWeights.size() * 9);
        for (const auto &corner : cityModel.groundWeights)
          for (const auto &weight : corner) for (const double value : weight) weights.push_back(float(value));
        put("weights", weights.data(), weights.size() * sizeof(float), "f4", json::array({3, 3}));
      }

      j["arrays"] = arrays;
      j["arrayFile"] = "city-model-c10.bin";
      j["groundMaterialIds"] = cityModel.groundMaterialIds;
      j["elevationLines"]=json::array();for(const auto &line:cityModel.elevationLines)j["elevationLines"].push_back(poly({line[0],line[1]}));
      j["groundPaint"]=json::array();
      if(cityMeshSettings.textureStreets && !site.landforms.cells.empty())
        for(const auto &paint:cityModel.surfaces.influences) {
          const auto ringJson=[](const procedural::geom2::Ring &ring){json a=json::array();for(auto p:ring)a.push_back({procedural::geom2::toMetres(p.x),procedural::geom2::toMetres(p.y)});return a;};
          json holes=json::array();for(const auto &h:paint.polygon.holes)holes.push_back(ringJson(h));
          j["groundPaint"].push_back({{"outer",ringJson(paint.polygon.outer)},{"holes",holes},{"channel",paint.channel}});
        }
      j["textureManifest"] = "assets/procedural/textures/medieval-pixel/materials.json";
      j["sourcePipeline"]="world → site → territory → fabric → city completion → city mesh";
      j["wallAccessLinks"]=json::array();
      for(const auto &link:cityModel.wallAccessLinks)
        j["wallAccessLinks"].push_back({{"wing",link.wing},{"floor",link.floor},{"clearWidth",link.clearWidth},{"path",poly(link.path)}});
      j["waterways"]=json::array();for(const auto &w:cityComplete.waterways)j["waterways"].push_back({{"route",w.route},{"edges",w.edges},{"path",poly({w.a,w.b})},{"halfWidth",w.halfWidth},{"waterLevel",w.waterLevel},{"bedLevel",w.bedLevel},{"crossings",w.bridges},{"wallPortals",w.portals.size()}});
      j["historicalBindings"]=json::array();
      for(const auto &binding:cityComplete.historicalLots) {
        const auto physical=std::find_if(cityModel.buildings.begin(),cityModel.buildings.end(),[&](const auto &b){return b.id==binding.lot;});
        j["historicalBindings"].push_back({{"historicalBuilding",binding.building},{"lot",binding.lot},
          {"generatedBuilding",physical==cityModel.buildings.end()?json(nullptr):json(physical->id)}});
      }
      j["generation"]={{"seed",settings.brief.seed},{"population",population},{"reliefMetres",settings.brief.reliefMetres},{"siteCell",site.cell},{"cityRegionOnly",cityRegionOnly}};
      j["generation"]["raisedDistrictsEnabled"]=raisedDistricts;
      j["generation"]["raisedDistrictCount"]=cityRelief.platforms.size();
      j["generation"]["hillsMetres"]=site.landforms.cells.empty()?hills:0;
      j["generation"]["landformReliefMetres"]=site.landforms.reliefMetres;
      j["generation"]["terrainCrownFraction"]=site.landforms.facetCrownFraction;
      j["generation"]["polygonTerrain"]=cityMeshSettings.polygonTerrain && !site.landforms.cells.empty();
      j["generation"]["landformSummit"]=site.landforms.summit;
      j["landforms"]=json::array();
      for(const auto &cell:site.landforms.cells)j["landforms"].push_back({{"id",cell.id},{"boundary",poly(cell.boundary)},{"shelf",cell.shelf},{"uplift",cell.uplift},{"uphill",cell.uphill}});
      j["landBlocks"]=json::array();for(const auto &b:cityRelief.landBlocks)j["landBlocks"].push_back({{"block",b.block},{"kind",int(b.kind)},{"boundary",poly(b.boundary)},{"lots",b.lots},{"grade",b.maximumGrade},{"minimum",b.minimum},{"maximum",b.maximum}});
      j["platforms"]=json::array();
      for(const auto &deck:cityRelief.platforms)j["platforms"].push_back({{"block",deck.block},{"level",deck.level},{"boundary",poly(deck.boundary)}});
      j["infrastructureOnly"]=cityMeshSettings.infrastructureOnly;
      j["roadOwners"]=cityModel.roadOwners;
      j["roadSegments"]=cityModel.roadSegments;
      j["buildings"] = json::array();
      for (const auto &b : cityModel.buildings)
        j["buildings"].push_back({{"id",b.id},{"source",b.source},{"kind",cityModelBuildingKindName(b.kind)},{"footprint",poly(b.footprint)},
                                   {"base",b.base},{"wallHeight",b.wallHeight},{"roofHeight",b.roofHeight},{"central",b.central},{"wallAttached",b.wallAttached},
                                   {"pieces",b.pieces},{"contacts",b.contacts},{"rooms",b.rooms},{"roomConnections",b.roomConnections},
                                   {"windows",b.windows},{"chapels",b.chapels},{"chancels",b.chancels},{"entranceLayout",int(b.entranceLayout)},
                                   {"entranceRise",b.entranceRise},{"entranceCells",b.entranceCells},{"exteriorDoors",poly(b.exteriorDoors)},{"jettyMetres",b.jettyMetres},{"interiorViews",poly(b.interiorViews)},{"trade",b.trade}});
      for (size_t i=0;i<cityModel.buildings.size();++i) {
        // The rooms, by use and kind, so a person's day can be bound to a room of the generated city.
        json rooms=json::array();for(const auto &room:cityModel.buildings[i].roomList)rooms.push_back(roomJson(room));
        j["buildings"][i]["roomList"]=rooms;
      }
      j["blockStrategies"]=json::object();
      static constexpr const char *strategyNames[]={"unplanned","frontage","courtyard","crofts","infill","common"};
      for(const auto &r:cityComplete.skeleton.regions)if(r.kind==CityRegionKind::Block) {
        const auto key=strategyNames[size_t(r.blockStrategy)];
        j["blockStrategies"][key]=j["blockStrategies"].value(key,0)+1;
      }
      j["gatewayHouses"]=cityModel.gatewayHouses;
      j["publicSpaceCount"]=cityComplete.skeleton.publicSpaces.size();
      j["surfaces"]=json::array();
      j["entranceStructures"]=json::array();
      for(const auto &b:cityModel.buildings)for(const auto &s:b.siteAccess.slabs)
        j["entranceStructures"].push_back({{"building",b.id},{"polygon",poly(s.polygon)},{"bottom",b.base+s.bottom},{"top",b.base+s.top},{"landing",s.landing},{"tread",s.tread}});
      for(const auto &s:cityModel.surfaces.pieces){
        json outer=json::array(),holes=json::array();
        for(auto p:s.polygon.outer)outer.push_back({procedural::geom2::toMetres(p.x),procedural::geom2::toMetres(p.y)});
        for(const auto &ring:s.polygon.holes){json h=json::array();for(auto p:ring)h.push_back({procedural::geom2::toMetres(p.x),procedural::geom2::toMetres(p.y)});holes.push_back(h);}
        j["surfaces"].push_back({{"kind",int(s.kind)},{"owner",s.owner},{"segment",s.segment},{"outer",outer},{"holes",holes}});
      }
      j["counts"]={{"walls",cityModel.wallTriangles},{"towers",cityModel.towerTriangles},{"gatehouses",cityModel.gatehouseTriangles},{"roads",cityModel.roadTriangles},
                     {"lots",cityModel.lotTriangles},{"compounds",cityModel.compoundTriangles},{"lotBlueprints",cityModel.lotBlueprints},{"lotStructures",cityModel.lotStructures},
                     {"lotMeshes",cityModel.lotMeshes},{"lotRejected",cityModel.lotRejected}};
      j["structure"]={{"pieces",cityModel.structuralPieces},{"contacts",cityModel.structuralContacts},{"rooms",cityModel.structuralRooms},{"roomConnections",cityModel.structuralRoomConnections}};
      j["rejections"]=cityModel.rejections;
      const auto &construction=cityModel.construction;
      const auto constructionId=[](CityConstructionId id){return json{{"kind",int(id.kind)},{"source",id.source}};};
      auto &blueprint=j["constructionBlueprint"];
      blueprint={{"revision",construction.revision},{"status","candidate"},
                 {"issues",json::array()},{"elements",json::array()},{"demand",json::array()}};
      for(const auto &issue:construction.issues)
        blueprint["issues"].push_back({{"code",issue.code},{"element",constructionId(issue.element)}});
      for(const auto &element:construction.elements)
        blueprint["elements"].push_back({{"id",constructionId(element.id)},{"owner",constructionId(element.owner)},
          {"grammar",int(element.grammar)},{"boundary",poly(element.boundary)},{"capacityArea",element.capacityArea}});
      for(const auto &demand:construction.demand)
        blueprint["demand"].push_back({{"building",demand.building},{"requiredArea",demand.requiredArea},
          {"allocation",constructionId(demand.allocation)},{"placed",demand.placed},{"unmetReason",demand.unmetReason}});

      writeCityInstances(output/"city-model-c10.json",cityModel,j);std::ofstream(output / "city-model-c10.json") << j.dump();
    }
  }
  // Block grammar (B1-B3) on the first street blocks of a planned layout, or
  // on ward pieces of an organic one; rendered as one SVG sheet.
  if (fabricOK && fabric.layoutAccepted)
  {
    std::vector<std::pair<uint64_t, std::vector<Position>>> candidates;
    for (const auto &blk : fabric.layout.blocks)
    {
      if (blk.boundary.outer.size() >= 3 && blk.courtyards.empty())
      {
        candidates.push_back({blk.id, blk.boundary.outer});
      }
    }
    if (candidates.empty())
    {
      for (const auto &ward : fabric.layout.wards)
      {
        for (const auto &piece : ward.pieces)
        {
          candidates.push_back({ward.id, piece});
        }
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](auto &a, auto &b) {
      return std::abs(signedAreaOf(a.second)) > std::abs(signedAreaOf(b.second));
    });
    generator::Ledger blockLedger;
    json blocksJson = json::array();
    const double sheet = 1400;
    std::vector<generator::BlockPlan> plans;
    for (size_t i = 0; i < candidates.size() && plans.size() < 4; ++i)
    {
      auto outline = candidates[i].second;
      if (outline.size() > 64 || std::abs(signedAreaOf(outline)) < 400)
      {
        continue;
      }
      if (signedAreaOf(outline) < 0)
      {
        std::reverse(outline.begin(), outline.end());
      }
      std::vector<bool> streetSides(outline.size(), false);
      for (size_t k = 0; k < outline.size(); ++k)
      {
        Position mid{(outline[k].x + outline[(k + 1) % outline.size()].x) * .5, 0, (outline[k].z + outline[(k + 1) % outline.size()].z) * .5};
        for (const auto &st : fabric.layout.streets)
        {
          for (size_t j = 1; j < st.path.size(); ++j)
          {
            auto a = st.path[j - 1], b = st.path[j];
            double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
            double t = l2 > 0 ? std::clamp(((mid.x - a.x) * dx + (mid.z - a.z) * dz) / l2, 0., 1.) : 0;
            if (std::hypot(mid.x - a.x - dx * t, mid.z - a.z - dz * t) <= st.halfWidth + 1.5)
            {
              streetSides[k] = true;
            }
          }
        }
      }
      generator::BlockGrammarSettings settings;
      settings.seed = settings.seed + candidates[i].first;
      generator::BlockPlan plan;
      std::string blockError;
      if (!generator::planBlockGrammar(candidates[i].first, outline, streetSides, fabric.terrain, settings, plan, blockLedger, &blockError))
      {
        std::cout << "block " << candidates[i].first << " grammar rejected: " << blockError << "\n";
        continue;
      }
      std::cout << "block " << candidates[i].first << " cells=" << plan.cells.size() << " lots=" << plan.lots << " alleys=" << plan.alleys << " stairs=" << plan.stairs << " courts=" << plan.courts
                << " streets=" << plan.streets << " inaccessible=" << plan.inaccessibleLots << " restarts=" << plan.restarts << " solved=" << plan.solved << "\n";
      json cells = json::array();
      for (const auto &c : plan.cells)
      {
        cells.push_back({{"id", c.id}, {"ring", poly(c.ring)}, {"tile", int(c.tile)}, {"elevation", c.elevation}, {"accessible", c.accessible}});
      }
      blocksJson.push_back(
          {{"block", candidates[i].first}, {"outline", poly(outline)}, {"cells", cells}, {"lots", plan.lots}, {"alleys", plan.alleys}, {"stairs", plan.stairs}, {"courts", plan.courts}, {"solved", plan.solved}});
      plans.push_back(std::move(plan));
    }
    if (!plans.empty())
    {
      double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
      for (auto &plan : plans)
      {
        for (auto &c : plan.cells)
        {
          for (auto v : c.ring)
          {
            x0 = std::min(x0, v.x);
            z0 = std::min(z0, v.z);
            x1 = std::max(x1, v.x);
            z1 = std::max(z1, v.z);
          }
        }
      }
      double scale = sheet / std::max(x1 - x0, z1 - z0);
      Svg svg(sheet, sheet, scale);
      svg.rect(0, 0, sheet / scale, sheet / scale, "#f4f1e8");
      for (auto &plan : plans)
      {
        for (auto &c : plan.cells)
        {
          std::string fill = c.tile == generator::BlockTile::Street  ? "#c9b28a"
                             : c.tile == generator::BlockTile::Alley ? "#e6dcc3"
                             : c.tile == generator::BlockTile::Stair ? "#b07a4a"
                             : c.tile == generator::BlockTile::Lot   ? (c.accessible ? "#8a5a3a" : "#d04040")
                             : c.tile == generator::BlockTile::Court ? "#9fb98a"
                                                                     : "#ffffff";
          Poly local;
          for (auto v : c.ring)
          {
            local.push_back({v.x - x0, 0, v.z - z0});
          }
          svg.polygon(local, fill, "#3a2a1a", .5, 1);
        }
      }
      svg.text({10, 0, (z1 - z0) - 12 / scale}, "block grammar: lots (brown, red = no access), alleys (pale), stairs (dark), courts (green), street cells (tan)", 14, "#000", "start");
      std::ofstream(output / "blocks.svg") << svg.finish();
      std::ofstream(output / "blocks.json") << blocksJson.dump(2);
    }
  }
  // Compound sheet: each castle/manor/church/hall precinct with walls, towers and planned wings.
  if (fabricOK && !fabric.compounds.empty())
  {
    const double panel = 700;
    const size_t cols = std::min<size_t>(3, fabric.compounds.size());
    const size_t rowsN = (fabric.compounds.size() + cols - 1) / cols;
    Svg svg(panel * cols, panel * rowsN, 1);
    svg.rect(0, 0, panel * cols, panel * rowsN, "#f4f1e8");
    for (size_t i = 0; i < fabric.compounds.size(); ++i)
    {
      const auto &c = fabric.compounds[i];
      double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
      for (auto v : c.precinct)
      {
        x0 = std::min(x0, v.x);
        z0 = std::min(z0, v.z);
        x1 = std::max(x1, v.x);
        z1 = std::max(z1, v.z);
      }
      const double scale = (panel - 80) / std::max(x1 - x0, z1 - z0);
      const double ox = (i % cols) * panel + 40, oz = (rowsN - 1 - i / cols) * panel + 40;
      auto map = [&](Position p) {
        return Position{ox + (p.x - x0) * scale, 0, oz + (p.z - z0) * scale};
      };
      auto mapPoly = [&](const Poly &p) {
        Poly out;
        for (auto v : p)
        {
          out.push_back(map(v));
        }
        return out;
      };
      svg.polygon(mapPoly(c.precinct), "#e8e0c8", "#8a7a5a", 1, 1);
      for (const auto &court : c.courts)
      {
        svg.polygon(mapPoly(court), "#d8cfa8", "none", 0, .8);
      }
      for (const auto &wing : c.wings)
      {
        svg.polygon(mapPoly(wing.brief.shell), wing.planned ? "#8a5a3a" : "#d04040", "#2a1a0a", 1, 1);
        for (const auto &room : wing.plan.rooms)
        {
          if (room.floor == 0)
          {
            svg.polygon(mapPoly(room.boundary), "#c9a878", "#5a3a1a", .6, 1);
          }
        }
        auto cshell = centroid(wing.brief.shell);
        svg.text(map(cshell), wing.role, 13, "#fff");
      }
      for (const auto &tower : c.towers)
      {
        svg.polygon(mapPoly(tower.footprint), "#404040", "#000", .6, 1);
      }
      for (const auto &wall : c.walls)
      {
        svg.polyline(mapPoly({wall.a, wall.b}), wall.gate ? "#f0d080" : "#202020", std::max(2., wall.thickness * scale));
      }
      const char *kindName = c.kind == generator::CompoundKind::Castle ? "castle" : c.kind == generator::CompoundKind::Manor ? "manor" : c.kind == generator::CompoundKind::Church ? "church" : "hall";
      svg.text(
          {ox, 0, oz + (z1 - z0) * scale + 24},
          std::string(kindName) + " " + std::to_string(c.id) + ": " + std::to_string(c.plannedWings) + "/" + std::to_string(c.wings.size()) + " wings planned, " + std::to_string(c.towers.size()) + " towers",
          14,
          "#000",
          "start");
    }
    std::ofstream(output / "compounds.svg") << svg.finish();
  }
  // HTML.
  {
    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>World settlement W0 "
            "preview</title><style>body{font-family:Helvetica,Arial,sans-serif;margin:20px;color:#222}table{border-collapse:collapse;margin:8px 0}td,th{border:1px solid #ccc;padding:3px "
            "8px;font-size:13px;text-align:left}img{max-width:100%;border:1px solid #999}.legend span{display:inline-block;width:14px;height:14px;margin-right:4px;vertical-align:middle;border:1px solid "
            "#666}</style></head><body>";
    html << "<h1>Actual-world settlement, W0 input adapter</h1><p>Seed " << settings.brief.seed << ", " << extent / 1000 << " km world, " << settings.cellSpacingMetres << " m leaf cells, relief "
         << settings.brief.reliefMetres << " m. Site cell " << site.cell << " in " << site.regionName << ", population brief " << site.demand.population << ".</p>";
    html << "<h2>World overview</h2><p>Leaf cells by biome, rivers by discharge. Green dots: accepted site candidates; red: rejected with a recorded reason. Orange box: the settlement domain (the urban cell plus its "
            "neighbouring map cells).</p><img src=\"overview.svg\">";
    html << "<h2>Settlement domain (local frame, " << site.bounds.maxX << " x " << site.bounds.maxZ
         << " m)</h2><p class=\"legend\"><span style=\"background:#7fb2e5\"></span>explicit water <span style=\"background:#c49a6c\"></span>steep <span style=\"background:#b8a48a\"></span>rough <span "
            "style=\"background:#e6dca0\"></span>poor soil <span style=\"background:#9fc7b8\"></span>saturated <span style=\"background:#d5d5d5\"></span>unreachable <span style=\"background:#4f7a3a\"></span>forest "
            "region <span style=\"background:#8a8a8a\"></span>rock region <span style=\"background:#e9d58a\"></span>allocated farmland <span style=\"background:#d9b85c\"></span>crop field <span "
            "style=\"background:#b28b5a\"></span>lane <span style=\"background:#a0703c\"></span>farmyard <span style=\"background:#e07b5f\"></span>residential <span style=\"background:#d9a441\"></span>craftsmen <span "
            "style=\"background:#c56f9a\"></span>worker housing <span style=\"background:#8e6bd6\"></span>temple <span style=\"background:#4a8fd6\"></span>civic. White circles: gate cells; yellow: road exits; red: site "
            "centre; dashed black: planning access; dashed brown: farm access; thin brown: elevation contours.</p><img src=\"domain.svg\">";
    html << "<h2>Ledger</h2><table><tr><th>Metric</th><th>Value</th></tr>";
    html << "<tr><td>Candidates evaluated</td><td>" << site.candidates.size() << "</td></tr><tr><td>Planning cells</td><td>" << site.brief.input.cells.size() << "</td></tr><tr><td>Hinterland regions</td><td>"
         << site.regions.size() << "</td></tr><tr><td>Road exits / gates</td><td>" << site.brief.input.ruralSources.size() << " / " << site.brief.input.gateCells.size() << "</td></tr>";
    if (composed)
    {
      html << "<tr><td>Districts allocated</td><td>" << territory.regions.districts.size() << " (" << territory.regions.assignedArea << " m&sup2;)</td></tr><tr><td>Farm patches / fields / crop area</td><td>"
           << territory.rural.patches.size() << " / " <<
          [&] {
            size_t n = 0;
            for (auto &f : territory.fields)
            {
              n += f.fields.size();
            }
            return n;
          }()
           << " / " << territory.cropArea / 1e4 << " ha</td></tr><tr><td>Farmsteads</td><td>" << territory.holdings.size() << "</td></tr><tr><td>Rural rejections</td><td>" << territory.rural.rejections.size()
           << "</td></tr>";
    }
    else
    {
      html << "<tr><td>Territory</td><td>rejected: " << error << "</td></tr>";
    }
    html << "</table><h3>Rejected site candidates (first 12)</h3><table><tr><th>Cell</th><th>Score</th><th>Reason</th></tr>";
    size_t shown = 0;
    for (const auto &c : site.candidates)
    {
      if (!c.accepted && shown++ < 12)
      {
        html << "<tr><td>" << c.cell << "</td><td>" << c.score << "</td><td>" << c.rejection << "</td></tr>";
      }
    }
    html << "</table>";
    if (composed)
    {
      std::map<std::string, int> kinds;
      for (auto r : territory.rural.rejections)
      {
        auto pos = r.find(' ', r.find("region ") + 7);
        kinds[pos == std::string::npos ? r : "original region N " + r.substr(pos + 1)]++;
      }
      html << "<h3>Rural rejections by kind</h3><table><tr><th>Kind</th><th>Count</th></tr>";
      for (auto &[k, v] : kinds)
      {
        html << "<tr><td>" << k << "</td><td>" << v << "</td></tr>";
      }
      html << "</table>";
    }
    html << "</body></html>";
    std::ofstream(output / "index.html") << html.str();
  }
  std::cout << "wrote " << output << "\n";
  report["jobs"] = json::array();
  for (auto &job : jobs.jobs)
  {
    report["jobs"].push_back(
        {{"name", job.name}, {"built", job.built}, {"error", job.error}, {"bounds", {job.minX, job.minZ, job.maxX, job.maxZ}}, {"faces", job.built ? job.result.assets.levels[0].world.map.faces.size() : 0}});
  }
  std::vector<bool> runtimeTiles;
  for (const auto &job : jobs.jobs) runtimeTiles.push_back(job.built);
  auto release = assessCityRelease(cityComplete,cityRelief,cityModel,siteSettings.historicalBuildings,runtimeTiles);
  // Enclosure records are exported for diagnosis. Physical projection is not
  // implemented yet; do not imply that generic walls preserve historical eras.
  if (historicalProvenance.is_null() || historicalProvenance["defenceCircuits"].is_null())
    release.failures.push_back("historical-enclosures-missing");
  else if (historicalProvenance["defenceCircuits"].empty() && historicalProvenance.value("defenceSource","")=="fabric-plan")
    ; // No dated circuits; the planned curtain stands in for them.
  else if (!cityComplete.historicalGrowth || !cityComplete.historicalGrowth->omissions.empty())
    release.failures.push_back("historical-enclosures-unresolved");
  release.accepted = release.failures.empty();
  report["historyProvenance"] = historicalProvenance;
  report["release"] = {{"schemaVersion",1},{"geometryAccepted",release.accepted},
    {"historicalRequested",release.requested},{"historicalGenerated",release.generated},
    {"missingHistoricalBuildings",release.missingBuildings},{"failures",release.failures},
    {"viewsReviewed",false},{"releaseAccepted",false}};
  stage("reports and renders");
  std::ofstream(output / "settlement.json") << report.dump(2);
  stage("settlement.json");
  if(!composed || !modelOK)return 3;
  if(voxelAssemblyAudit && (voxelMissingCount || voxelOverlapCount || voxelModelMismatchCount || voxelDuplicateBuildingIds))return 4;
  // All world configurations require all tiles, not only opt-in landforms.
  if(!runtimeOK)return 4;
  if(releaseCheck && !release.accepted)return 5;
  return 0;
}
