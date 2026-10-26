#include "procedural/geometry/TerrainMapSource.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include <cmath>
#include <iostream>

using namespace procedural;
using namespace procedural::world;
using namespace procedural::geometry;
namespace
{
int failures = 0;
void expect(bool value, const char *message)
{
  std::cout << (value ? "PASS " : "FAIL ") << message << '\n';
  failures += !value;
}
MapExportBundle fixture(bool mountains = false)
{
  MapExportBundle bundle;
  bundle.resolution = 4; bundle.extentMetres = 4000;
  bundle.pageSizePixels = 2; bundle.gutterPixels = 1; bundle.maximumPageLevel = 1;
  const auto add = [&](std::string name, MapExportFormat format, MapExportFilter filter,
                       double scale, double offset, auto value)
  {
    MapExportField field;
    field.name = name; field.available = true; field.width = field.height = 4;
    field.format = format; field.filter = filter; field.scale[0] = scale; field.offset[0] = offset;
    for (uint32_t pageY = 0; pageY < 2; ++pageY)
      for (uint32_t pageX = 0; pageX < 2; ++pageX)
      {
        MapExportPage page;
        page.address = {1, pageX, pageY}; page.width = page.height = 4;
        const auto bytes = mapExportTexelBytes(format);
        page.data.resize(16 * bytes);
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x)
          {
            const int sx = std::clamp(int(pageX * 2) + x - 1, 0, 3);
            const int sy = std::clamp(int(pageY * 2) + y - 1, 0, 3);
            const uint32_t sample = value(sx, sy);
            for (uint32_t byte = 0; byte < bytes; ++byte)
              page.data[(y * 4 + x) * bytes + byte] = (sample >> (byte * 8)) & 255;
          }
        field.pages.push_back(std::move(page));
      }
    bundle.fields.push_back(std::move(field));
  };
  add("refined_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear, 3040, -957,
      [mountains](int x, int y) { return (mountains ? 40000 : 22000) + x * 150 + y * 70; });
  add("relief_roughness", MapExportFormat::RG16Float, MapExportFilter::Linear, 1, 0,
      [](int, int) { return 0x380060b0; }); // 600 m relief, 0.5 roughness
  add("base_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear, 3040, -937,
      [mountains](int x, int y) { return (mountains ? 40000 : 22000) + x * 150 + y * 70; });
  add("water_level", MapExportFormat::R16Unorm, MapExportFilter::Linear, 20, 0,
      [](int x, int) { return x == 0 ? 0 : 32767; });
  add("river", MapExportFormat::RG16Float, MapExportFilter::Linear, 1, 0,
      [](int, int) { return 0x00007bff; });
  add("water_type", MapExportFormat::R8Uint, MapExportFilter::Nearest, 1, 0,
      [](int x, int) { return x == 0 ? 1 : 0; });
  add("biome_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, 1, 0,
      [](int x, int) { return uint32_t(x == 0 ? Biome::Ocean : Biome::Grassland); });
  add("landform_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, 1, 0,
      [](int, int) { return uint32_t(CellType::Plain); });
  add("region_id", MapExportFormat::R32Uint, MapExportFilter::Nearest, 1, 0,
      [](int x, int y) { return x + y * 4; });
  return bundle;
}
}
int main()
{
  const RidgeNode ridgeA{{1000,1000},0,2000,5000}, ridgeB{{3000,1000},1,2000,5000};
  expect(std::abs(ridgeProfileHeight({6000,1000-.001},ridgeA,ridgeB)-
      ridgeProfileHeight({6000,1000+.001},ridgeA,ridgeB)) < .01,
      "asymmetric ridge end caps have no straight extension seam");
  auto bundle = fixture();
  TerrainMapSource source; source.streamPower.enabled=false;
  std::string error;
  expect(source.initialize(bundle, &error), "export adapter initializes");
  expect(source.elevationField == "refined_elevation", "raw refined elevation remains available for sampler diagnostics");
  bool matches = true, ocean = true, sampled = true;
  for (double x : {0.0, 37.5, 499.0, 500.0, 999.0, 1000.0, 1999.0, 2000.0, 2001.0, 3555.0, 4000.0})
    for (double y : {0.0, 500.0, 777.0, 1999.0, 2000.0, 3500.0, 4000.0})
    {
      const Point2 p(x, y);
      MapExportFieldSample reference;
      sampled &= sampleMapExportFieldAtWorld(bundle, "refined_elevation", p, 1, reference, &error);
      matches &= std::abs(source.groundHeight(p) - reference.values[0]) < 1e-9;
      if (source.waterType(p)) ocean &= source.height(p) == 0.0;
    }
  expect(sampled && matches, "physical scale, offset, cell centres, clamping and page seams match the voxel sampler");
  expect(ocean, "ocean uses its water level instead of the encoded bed elevation");
  expect(source.maximumElevation < 100.0, "the input's gentle lowlands are not stretched to a mountain-height range");
  expect(source.constructionField == "base_elevation" && source.height({3500,3500}) > source.groundHeight({3500,3500}) + 15,
      "surface construction uses clean base data instead of refined incision artefacts");
  auto chains = source.constraintChains();
  expect(std::all_of(chains.begin(), chains.end(), [](const auto &chain) { return !chain.hasElevation; }),
      "semantic topology cannot override final eroded height");
  TerrainMeshSettings settings;
  settings.tileSizeMetres = 2000; settings.preserveChainVertices = true;
  settings.maximumConstraintSegmentMetres = 200;
  settings.borderSpacingMetres = 200; settings.maximumRefinementRounds = 2;
  settings.poissonSpacingMetres = 400; settings.poissonLayers = 1;
  settings.workerCount = 1;
  TerrainMesh serial, parallel;
  expect(buildTerrainMesh(chains, [&source](Point2 p) { return source.height(p); },
      source.materialFunction(serial.palette), source.extentMetres, settings, serial, &error), "exported terrain meshes");
  double maximumError = 0;
  for (const auto &tile : serial.tiles)
    for (const auto &v : tile.vertices)
      maximumError = std::max(maximumError, std::abs(v.y - source.height({v.x, v.z})));
  expect(maximumError < 1e-9, "every mesh vertex follows the source height without beautification");
  settings.workerCount = 4;
  expect(buildTerrainMesh(chains, [&source](Point2 p) { return source.height(p); },
      source.materialFunction(parallel.palette), source.extentMetres, settings, parallel, &error), "parallel export mesh builds");
  expect(serial.signature() == parallel.signature(), "source import and meshing are deterministic");
  source.flattenOcean(serial);
  size_t oceanFaces = 0;
  bool flatWater = true, unchangedLand = true;
  for (const auto &tile : serial.tiles)
    for (size_t face = 0; face < tile.materials.size(); ++face)
    {
      const bool water = serial.palette.entries()[tile.materials[face]].cover == Biome::Ocean;
      oceanFaces += water;
      for (size_t corner = 0; corner < 3; ++corner)
      {
        const auto &v = tile.vertices[tile.indices[face * 3 + corner]];
        if (water) flatWater &= v.y == source.seaLevelMetres;
        else unchangedLand &= std::abs(v.y - source.height({v.x, v.z})) < 1e-9;
      }
    }
  expect(oceanFaces > 0 && flatWater, "all ocean faces stay flat including vertices shared with elevated coastal land");
  expect(unchangedLand, "splitting ocean vertices preserves the land surface");
  MapGraph graph;
  WorldBrief brief; brief.extentMetres = 4000; graph.setBrief(brief);
  MapNode sea; sea.type = CellType::Ocean; sea.biome = uint8_t(Biome::Ocean);
  sea.area = {{0, 0}, {1000, 0}, {1800, 4000}, {0, 4000}}; sea.site = {400, 2000};
  graph.addNode(sea);
  MapNode desert; desert.land = true; desert.type = CellType::Plain; desert.biome = uint8_t(Biome::Desert);
  desert.area = {{1000, 0}, {2500, 0}, {2900, 4000}, {1800, 4000}}; desert.site = {2100, 2000};
  graph.addNode(desert);
  MapNode forest = desert; forest.biome = uint8_t(Biome::TemperateForest);
  forest.area = {{2500, 0}, {4000, 0}, {4000, 4000}, {2900, 4000}}; forest.site = {3500, 2000};
  graph.addNode(forest);
  MapExportFeatures features;
  features.rivers.segments.push_back({{2000, 1000}, {2400, 3000}, 200, 2, 1, false});
  source.setGeometry(graph, features);
  expect(source.waterType({1200, 3000}) == 1 && source.waterType({2000, 3000}) == 0,
      "diagonal vector coastline overrides square raster water labels");
  expect(source.biome({2600, 2000}) == uint8_t(Biome::Desert) &&
      source.biome({2800, 2000}) == uint8_t(Biome::TemperateForest),
      "biomes follow the constrained vector edge instead of nearest texture pixels");
  expect(std::abs(source.height({1400, 2000}) - source.seaLevelMetres) < 1e-6 &&
      std::abs(source.height({1400.01, 2000}) - source.seaLevelMetres) < 1e-6,
      "shore ground meets the water level continuously on a slanted coast");
  MaterialEntry desertCover, forestCover;
  desertCover.cover = Biome::Desert; forestCover.cover = Biome::TemperateForest;
  expect(source.surfaceColour({2700,2000},desertCover) == source.surfaceColour({2700,2000},forestCover),
      "shared boundary vertices receive the same blended cover color from either material");
  MaterialEntry waterCover; waterCover.role = SurfaceRole::Water; waterCover.cover = Biome::Ocean;
  expect(source.surfaceColour({1400,2000},waterCover) == materialColour(waterCover),
      "biome color transitions cannot tint or deform ocean surfaces");
  bool downhill = true, carved = false;
  double previous = 1e9;
  for (uint32_t i = 0; i <= 40; ++i)
  {
    const double t = double(i) / 40;
    const Point2 p(2000 + 400 * t, 1000 + 2000 * t);
    const double h = source.height(p);
    downhill &= h <= previous + 1e-8;
    carved |= h < source.coastalHeight(p) - 0.5;
    previous = h;
  }
  expect(downhill && carved, "connected river beds carve below terrain and never rise downstream");
  TerrainMapSource mountains, repeated; mountains.streamPower.enabled=repeated.streamPower.enabled=false;
  auto mountainBundle = fixture(true);
  expect(mountains.initialize(mountainBundle, &error) && repeated.initialize(mountainBundle, &error),
      "authored mountain relief initializes");
  mountains.setGeometry(graph, {});
  repeated.setGeometry(graph, {});
  expect(mountains.maximumMountainDetailMetres > 1 && source.maximumMountainDetailMetres == 0,
      "branching ridge detail affects high-relief uplands while gentle lowlands remain unchanged");
  expect(mountains.height({2500, 2500}) == repeated.height({2500, 2500}) && mountains.height({400, 2500}) == 0,
      "mountain construction is deterministic and leaves the ocean flat");
  // Corrupt only the refined field with alternating high/low sample values.
  // The clean base and therefore the reconstructed surface must be invariant.
  for (auto &page : mountainBundle.field("refined_elevation")->pages)
    for (size_t at = 0; at < page.data.size(); at += 2)
    { page.data[at] = 0; page.data[at+1] = (at/2)%2 ? 250 : 1; }
  TerrainMapSource contaminated; contaminated.streamPower.enabled=false;
  expect(contaminated.initialize(mountainBundle, &error), "striped refined diagnostic fixture loads");
  contaminated.setGeometry(graph, {});
  bool independent = true;
  for (double y = 250; y < 4000; y += 250) for (double x = 250; x < 4000; x += 250)
    independent &= contaminated.height({x,y}) == mountains.height({x,y});
  expect(independent, "refined raster stripes cannot leak into reconstructed geometry or ridge placement");
  bundle.field("refined_elevation")->available = false;
  expect(source.initialize(bundle, &error) && source.elevationField == "base_elevation", "base elevation is used only when the refined field is unavailable");
  bundle.field("water_type")->available = false;
  expect(!source.initialize(bundle, &error), "missing required maps produce an error instead of different terrain");
  {
    constexpr uint32_t side=16;
    std::vector<double> slope(side*side), rain(side*side), area(side*side), next;
    std::vector<uint8_t> outlets(side*side);
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x)
    {
      const size_t id=y*side+x; slope[id]=1000-y*20;
      outlets[id]=x==0||y==0||x+1==side||y+1==side;
      rain[id]=area[id]=outlets[id]?0:1;
    }
    for(int i=0;i<40;++i)
    { streamPowerDrainageStep(side,100,slope,outlets,rain,area,next); area.swap(next); }
    double discharged=0;
    for(size_t i=0;i<area.size();++i)if(outlets[i])discharged+=area[i];
    expect(std::abs(discharged-196)<.001, "paper Lp drainage converges and conserves contributing catchment area");
    const auto original=slope;
    std::vector<double> uplift(slope.size(),.001),hardness(slope.size(),1);
    StreamPowerSettings settings; settings.iterations=300; settings.maximumTimeStep=100;
    auto duplicate=slope;
    const auto report=erodeStreamPower(side,100,uplift,rain,hardness,outlets,slope,settings);
    erodeStreamPower(side,100,uplift,rain,hardness,outlets,duplicate,settings);
    bool stable=true,frozen=true;
    for(size_t i=0;i<slope.size();++i)
    { stable &= std::isfinite(slope[i]) && slope[i]>=699; if(outlets[i])frozen &= slope[i]==original[i]; }
    expect(report.maximumCutMetres>10 && stable && frozen, "stream power incises the whole dry domain while water outlets remain fixed");
    expect(slope==duplicate, "paper Jacobi drainage and erosion are deterministic");
  }
  {
    TerrainMapSource scaled, native;
    scaled.horizontalScale=2; scaled.streamPower.enabled=native.streamPower.enabled=false;
    expect(scaled.initialize(fixture(),&error)&&native.initialize(fixture(),&error)&&
        scaled.extentMetres==8000&&scaled.groundHeight({3000,3000})==native.groundHeight({1500,1500}),
        "world expansion scales horizontal registration without stretching elevation");
  }
  constexpr uint32_t erosionSide = 48;
  std::vector<double> mound(erosionSide * erosionSide), mask(mound.size());
  for (uint32_t y = 0; y < erosionSide; ++y) for (uint32_t x = 0; x < erosionSide; ++x)
  {
    const size_t at = y * erosionSide + x;
    mound[at] = 500 + 180 * std::exp(-((double(x)-24)*(x-24.0)+(double(y)-24)*(y-24.0))/140.0) + y;
    mask[at] = x > 4 && x < 43 && y > 4 && y < 43 ? 1 : 0;
  }
  const auto original = mound;
  auto duplicate = mound;
  HydraulicErosionSettings hydraulicSettings;
  hydraulicSettings.dropletsPerCell = 2;
  const auto sediment = erodeMountainHydraulics(erosionSide, 25, 17, mask, mound, hydraulicSettings);
  erodeMountainHydraulics(erosionSide, 25, 17, mask, duplicate, hydraulicSettings);
  bool untouched = true;
  double volumeChange = 0;
  for (size_t i = 0; i < mound.size(); ++i)
  { if (!mask[i]) untouched &= mound[i] == original[i]; volumeChange += (original[i]-mound[i])*625; }
  expect(sediment.erodedCubicMetres > 0 && sediment.depositedCubicMetres > 0 && sediment.maximumCutMetres > 0,
      "hydraulic particles erode mountain slopes and deposit carried sediment");
  expect(untouched && mound == duplicate, "hydraulic erosion is deterministic and freezes plains outside the mountain mask");
  expect(std::abs(sediment.erodedCubicMetres-sediment.depositedCubicMetres-sediment.exportedCubicMetres) < 1e-5 &&
      std::abs(volumeChange-sediment.exportedCubicMetres) < 1e-5,
      "terrain change and sediment export conserve mass");
  MapGraph ridgeMap;
  WorldBrief ridgeBrief; ridgeBrief.extentMetres = 12000; ridgeMap.setBrief(ridgeBrief);
  for (uint32_t y = 0; y < 12; ++y) for (uint32_t x = 0; x < 12; ++x)
  {
    MapNode node; node.land = true; node.type = CellType::Mountain;
    node.site = {(x+.5)*1000, (y+.5)*1000};
    node.area = {{x*1000.0,y*1000.0},{(x+1)*1000.0,y*1000.0},
        {(x+1)*1000.0,(y+1)*1000.0},{x*1000.0,(y+1)*1000.0}};
    ridgeMap.addNode(node);
  }
  const auto ridgeEdges = buildEdgeSet(ridgeMap, 1e-6);
  const auto skeleton = buildMountainSkeleton(ridgeMap, ridgeEdges, [](Point2) {return 1800.0;},
      [](Point2) {return 600.0;}, 0, 7);
  bool branches = false, tree = true;
  std::vector<uint32_t> sets(skeleton.nodes.size());
  for (uint32_t i = 0; i < sets.size(); ++i) sets[i] = i;
  const auto root = [&](uint32_t i) {while(sets[i]!=i)i=sets[i];return i;};
  for (const auto &edge : skeleton.edges)
  {
    branches |= edge.level > 0;
    const auto a = root(edge.from), b = root(edge.to);
    tree &= a != b;
    sets[a] = b;
  }
  expect(skeleton.rangeCount > 0 && branches && !skeleton.passes.empty(),
      "mountain skeleton contains connected major spines, subordinate branches and explicit passes");
  expect(tree && skeleton.edges.size()+skeleton.rangeCount == skeleton.nodes.size(),
      "ridge hierarchy is an acyclic forest built from graph adjacency");
  {
    auto climateBundle=fixture();climateBundle.extentMetres=12000;
    TerrainMapSource climate;climate.variedBiomes=true;climate.streamPower.enabled=false;climate.hydraulics.enabled=false;
    expect(climate.initialize(climateBundle,&error),"climate fixture loads");climate.setGeometry(ridgeMap,{});
    std::array<bool,256> covers{};
    for(const auto &n:ridgeMap.nodes())covers[climate.biome(n.site)]=true;
    expect(std::count(covers.begin(),covers.end(),true)>=5 &&
        (covers[uint8_t(Biome::Desert)]||covers[uint8_t(Biome::SandDesert)]),
        "climate produces desert and multiple land biomes on constrained graph regions");
  }
  {
    auto bundle=fixture();bundle.extentMetres=12000;
    TerrainMapSource base,cliffs;
    base.streamPower.enabled=cliffs.streamPower.enabled=false;
    base.hydraulics.enabled=cliffs.hydraulics.enabled=false;
    cliffs.landforms.enabled=true;
    RockFormation f;f.topMetres=650;f.controls={{7600,3600},{10000,3600},{10000,6000},{7600,6000}};
    cliffs.landforms.authored={f};
    expect(base.initialize(bundle,&error)&&cliffs.initialize(bundle,&error),"authored spline source loads");
    expect(cliffs.rockFormations.size()==1 && cliffs.height({8800,4800})>640,
        "subcell spline shape survives source raster reconstruction");
    expect(cliffs.height({500,4800})==base.height({500,4800}),"spline landforms preserve ocean datum");
    expect(std::abs(cliffs.height({7000,10000})-base.height({7000,10000}))<1e-8,
        "analytic residual leaves distant terrain unchanged");
    auto chains=cliffs.constraintChains();bool brink=false,toe=false;
    for(const auto &c:chains){brink|=any(c.kind,ChainKind::Brink);toe|=any(c.kind,ChainKind::Toe);}
    expect(brink&&toe,"source exports spline brink and toe to CDT");
  }
  return failures ? 1 : 0;
}
