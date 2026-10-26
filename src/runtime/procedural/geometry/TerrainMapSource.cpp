#include "procedural/geometry/TerrainMapSource.hpp"
#include "procedural/geometry/TerrainGuides.hpp"
#include "procedural/geometry/TerrainRasterSource.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include "procedural/world/MapEdges.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::geometry
{
bool TerrainMapSource::load(const std::string &directory, std::string *error)
{
  world::MapExportBundle bundle;
  return world::readMapExportBundle(directory, bundle, error) && initialize(bundle, error);
}

bool TerrainMapSource::initialize(const world::MapExportBundle &bundle, std::string *error)
{
  const bool guides = guidedGeometry;
  const auto landformSettings = landforms;
  const auto hydraulicSettings = hydraulics;
  const auto streamSettings = streamPower;
  const auto settlementSettings = settlements;
  const auto parcelSettings = parcels;
  const auto detailSettings = detail;
  const auto scale = horizontalScale;
  const auto climate = variedBiomes;
  const auto densityOptions = meshDensity;
  const auto contourOptions = regionContours;
  const auto scarpSettings = scarps;
  const auto valleyMetres = riverValleyMetres;
  const auto valleyMaximum = riverValleyMaximumMetres;
  const auto ceilingFraction = reliefCeilingFraction;
  const auto blendMetres = naturalBlendMetres;
  // riverReaches is rebuilt by the geometry pass; nothing to carry.
  const auto warpFraction = regionWarpCellFraction;
  const auto warpMetres = regionWarpMetres;
  const auto warpScale = regionWarpScaleMetres;
  // `*this = {}` clears everything the bundle is about to refill, so every
  // caller-owned setting has to be carried across by hand. A new one left off
  // this list is silently reset to its default rather than failing to build.
  *this = {};
  streamPower = streamSettings; horizontalScale = scale; variedBiomes = climate;
  settlements = settlementSettings; parcels = parcelSettings; detail = detailSettings;
  hydraulics = hydraulicSettings; landforms = landformSettings; guidedGeometry=guides;
  regionWarpMetres = warpMetres; regionWarpScaleMetres = warpScale;
  regionWarpCellFraction = warpFraction;
  reliefCeilingFraction = ceilingFraction; naturalBlendMetres = blendMetres;
  riverValleyMetres = valleyMetres; riverValleyMaximumMetres = valleyMaximum;
  scarps = scarpSettings; regionContours = contourOptions; meshDensity = densityOptions;
  if (bundle.resolution < 2 || !(bundle.extentMetres > 0))
  {
    if (error) *error = "invalid world-map dimensions";
    return false;
  }
  const auto *refined = bundle.field("refined_elevation");
  elevationField = refined && refined->available ? "refined_elevation" : "base_elevation";
  if (!bundle.graphData.empty() && !world::decodeMapExportGraph(bundle, graph_, error)) return false;
  if (!bundle.featureData.empty() && !world::decodeMapExportFeatures(bundle, features_, error)) return false;
  extentMetres = bundle.extentMetres;
  resolution = bundle.resolution;
  seed = bundle.seed;
  const size_t count = size_t(resolution) * resolution;
  baseElevation_.resize(count); elevation_.resize(count); waterLevel_.resize(count);
  relief_.resize(count); roughness_.resize(count);
  riverDistance_.resize(count); discharge_.resize(count);
  water_.resize(count); biome_.resize(count); landform_.resize(count); region_.resize(count);
  std::array<std::string, 7> names{elevationField, "water_level", "river", "water_type", "biome_id", "landform_id", "region_id"};
  std::array<world::MapExportFieldCursor, 7> cursors{};
  world::MapExportFieldCursor reliefCursor, baseCursor;
  const auto *base = bundle.field("base_elevation");
  constructionField = base && base->available ? "base_elevation" : elevationField;
  const auto *reliefField = bundle.field("relief_roughness");
  for (uint32_t y = 0; y < resolution; ++y)
    for (uint32_t x = 0; x < resolution; ++x)
    {
      const size_t at = size_t(y) * resolution + x;
      const Point2 point((x + 0.5) * cellMetres(), (y + 0.5) * cellMetres());
      std::array<world::MapExportFieldSample, 7> samples{};
      for (size_t field = 0; field < names.size(); ++field)
        if (!world::sampleMapExportFieldAtWorld(bundle, names[field], cursors[field], point,
                bundle.maximumPageLevel, samples[field], error)) return false;
      elevation_[at] = samples[0].values[0];
      world::MapExportFieldSample baseSample;
      if (!world::sampleMapExportFieldAtWorld(bundle, constructionField, baseCursor, point,
              bundle.maximumPageLevel, baseSample, error)) return false;
      baseElevation_[at] = baseSample.values[0];
      waterLevel_[at] = samples[1].values[0];
      riverDistance_[at] = samples[2].values[0];
      discharge_[at] = samples[2].values[1];
      water_[at] = static_cast<uint8_t>(samples[3].values[0]);
      biome_[at] = static_cast<uint8_t>(samples[4].values[0]);
      landform_[at] = static_cast<uint8_t>(samples[5].values[0]);
      region_[at] = static_cast<uint32_t>(samples[6].values[0]);
      if (reliefField && reliefField->available)
      {
        world::MapExportFieldSample relief;
        if (!world::sampleMapExportFieldAtWorld(bundle, "relief_roughness", reliefCursor, point,
                bundle.maximumPageLevel, relief, error)) return false;
        relief_[at] = relief.values[0]; roughness_[at] = relief.values[1];
      }
    }

  // Relief is scaled to the world it is on.
  //
  // The exported field is authored for a continent, so it carries continental
  // relief - 4.4 km of it on the world this was measured on. Imported whole
  // onto a 48 km island that is a wall: the median land slope came out at
  // 0.365, more than half the land was too steep to parcel, and the interior
  // read as bare rock. The same field on a 256 km world is unremarkable.
  //
  // Mountains are not actually smaller on a small island - but a 48 km island
  // with a 4.4 km peak is Hawaii, and a generator asked for "a smaller world to
  // iterate on" should hand back a smaller version of the same landscape, not a
  // different and far more vertical one. Sea level is the fixed point, so only
  // the land is scaled and the coastline does not move.
  if (reliefCeilingFraction > 0)
  {
    double highest = seaLevelMetres;
    for (const double h : baseElevation_) highest = std::max(highest, h);
    const double ceiling = extentMetres * reliefCeilingFraction;
    const double rise = highest - seaLevelMetres;
    if (rise > ceiling && rise > 1e-6)
    {
      const double scale = ceiling / rise;
      const auto squash = [&](std::vector<double> &field)
      {
        for (double &h : field)
          if (h > seaLevelMetres) h = seaLevelMetres + (h - seaLevelMetres) * scale;
      };
      squash(baseElevation_); squash(elevation_);
    }
  }
  const auto range = std::minmax_element(elevation_.begin(), elevation_.end());
  minimumElevation = *range.first; maximumElevation = *range.second;
  const auto ocean = std::find(water_.begin(), water_.end(), uint8_t(1));
  if (ocean != water_.end()) seaLevelMetres = waterLevel_[size_t(ocean - water_.begin())];
  if (!(horizontalScale > 0) || !std::isfinite(horizontalScale))
  { if(error)*error="world scale must be positive and finite"; return false; }
  if (horizontalScale != 1)
  {
    const auto scalePoint = [&](Point2 &p) { p.x *= horizontalScale; p.y *= horizontalScale; };
    for(uint32_t id=0;id<graph_.nodeCount();++id)
    {
      auto &n=graph_.node(id); scalePoint(n.site);
      for(auto &p:n.area)scalePoint(p);
    }
    for(auto &line:features_.coastline)for(auto &p:line.points)scalePoint(p);
    for(auto &line:features_.contours.lines)for(auto &p:line.points)scalePoint(p);
    for(auto &r:features_.rivers.segments)
    { scalePoint(r.from); scalePoint(r.to); r.dischargeCumecs *= horizontalScale*horizontalScale; }
    for(auto &d:riverDistance_)d*=horizontalScale;
    for(auto &d:discharge_)d*=horizontalScale*horizontalScale;
    extentMetres *= horizontalScale;
    auto brief=graph_.brief(); brief.extentMetres=extentMetres; graph_.setBrief(brief);
  }
  try { setGeometry(std::move(graph_), std::move(features_)); }
  catch(const std::exception &e){if(error)*error=e.what();return false;}
  return true;
}

size_t TerrainMapSource::index(const Point2 point) const
{
  const auto coordinate = [&](double p)
  { return static_cast<uint32_t>(std::clamp(std::floor(p / cellMetres()), 0.0, double(resolution - 1))); };
  return size_t(coordinate(point.y)) * resolution + coordinate(point.x);
}

double TerrainMapSource::linear(const std::vector<double> &values, const Point2 point) const
{
  const double x = std::clamp(point.x / cellMetres() - 0.5, 0.0, double(resolution - 1));
  const double y = std::clamp(point.y / cellMetres() - 0.5, 0.0, double(resolution - 1));
  const uint32_t x0 = static_cast<uint32_t>(x), y0 = static_cast<uint32_t>(y);
  const uint32_t x1 = std::min(x0 + 1, resolution - 1), y1 = std::min(y0 + 1, resolution - 1);
  const double tx = x - x0, ty = y - y0;
  const auto at = [&](uint32_t a, uint32_t b) { return values[size_t(b) * resolution + a]; };
  return (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
      (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
}

double TerrainMapSource::groundHeight(Point2 p) const { return linear(elevation_, p); }
double TerrainMapSource::waterLevel(Point2 p) const
{
  // The sea datum is constant, even where filtering touches dry-land or lake
  // texels. Interpolating those into the ocean creates artificial shore slopes.
  return waterType(p) == 1 ? seaLevelMetres : linear(waterLevel_, p);
}
uint8_t TerrainMapSource::waterType(Point2 p) const
{
  if (!bucketWater_.empty() && bucketWater_[bucket(p)] != 255) return bucketWater_[bucket(p)];
  const auto id = regionNode(p);
  if (id == world::kInvalidNode) return water_[index(p)];
  const auto &node = graph_.node(id);
  return node.type == world::CellType::Lake ? 2 : !node.land ? 1 : 0;
}
uint8_t TerrainMapSource::biome(Point2 p) const
{
  uint8_t cover;
  if (!bucketBiome_.empty() && bucketBiome_[bucket(p)] != 255) cover = bucketBiome_[bucket(p)];
  else
  {
    const auto id = regionNode(p);
    cover = id == world::kInvalidNode ? biome_[index(p)] : graph_.node(id).biome;
  }
  if (!terrainElevation_.empty() && !waterType(p))
  {
    // These exact levels are also final-height CDT contour constraints.
    const double altitude = height(p) - seaLevelMetres;
    if (altitude >= 2800) return uint8_t(world::Biome::Glacier);
    if (altitude >= 2300) return uint8_t(world::Biome::ColdDesert);
    if (cover == uint8_t(world::Biome::Glacier)) return uint8_t(world::Biome::Tundra);
  }
  return cover;
}
uint8_t TerrainMapSource::landform(Point2 p) const
{
  if (!bucketLandform_.empty() && bucketLandform_[bucket(p)] != 255) return bucketLandform_[bucket(p)];
  const auto id = regionNode(p);
  return id == world::kInvalidNode ? landform_[index(p)] : uint8_t(graph_.node(id).type);
}
double TerrainMapSource::height(Point2 p) const
{
  if (waterType(p)) return waterLevel(p);
  // `waterType(p)` was just resolved as dry land on the line above.
  const double uncarved = coastalHeight(p, true);
  double ground = uncarved;
  const auto wet=rivers.sample(p);
  if(wet.inside)ground=std::min(ground,wet.level-wet.depth*(1-wet.lateral*wet.lateral));
  if (channelBuckets_.empty()) return ground;
  for (const auto id : channelBuckets_[bucket(p)])
  {
    const auto &s = channels_[id];
    const double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
    const double t = std::clamp(((p.x - s.a.x) * dx + (p.y - s.a.y) * dy) / std::max(1e-12, dx * dx + dy * dy), 0.0, 1.0);
    const double d = std::hypot(p.x - s.a.x - t * dx, p.y - s.a.y - t * dy);
    const double bed = s.bedA + (s.bedB - s.bedA) * t;
    // Widen incised valleys according to bank rise; a deep cut should have
    // sloping shoulders, not a narrow rectangular trench.
    const double outer = s.outerWidth;
    if (d >= outer) continue;
    const double half=s.halfWidth+(s.halfWidthB-s.halfWidth)*t;
    const double depth=s.depth+(s.depthB-s.depth)*t;
    if(wet.inside)continue;
    // The ribbon owns the wet bed; these spans only blend its outer banks.
    double cut = bed + depth;
    if (d > half)
    {
      const double u = (d - half) / (outer - half);
      cut += (uncarved - cut) * u * u * (3.0 - 2.0 * u);
    }
    ground = std::min(ground, cut);
  }
  return ground;
}

double TerrainMapSource::maximumTriangleArea(Point2 p) const
{
  if (!meshDensity.enabled) return 0.0;
  if (rivers.sample(p).inside) return meshDensity.river;
  if (coastDistance(p) < shoreWidth_ * .12) return meshDensity.shore;
  if (!parcelLayer.cells.empty())
  {
    const uint32_t cell = parcelLayer.cellAt(p);
    if (cell != kNoParcel)
      switch (parcelLayer.cells[cell].kind)
      {
      case ParcelKind::Road: case ParcelKind::Street:      return meshDensity.road;
      case ParcelKind::Shore:                              return meshDensity.shore;
      case ParcelKind::Riverbank:                          return meshDensity.river;
      case ParcelKind::Settlement: case ParcelKind::Yard:
      case ParcelKind::Market:     case ParcelKind::Citadel: return meshDensity.settlement;
      case ParcelKind::Rough:      case ParcelKind::Woodland: return meshDensity.wild;
      default:                                             return meshDensity.farmland;
      }
  }
  return meshDensity.wild;
}

double TerrainMapSource::toleranceScale(Point2 p) const
{
  if (waterType(p)) return 2.0;
  // What controls the silhouette is the constraint chains - brinks, toes, slope
  // ribs, contours and the two river banks are all explicit edges the
  // triangulator has to honour. Error refinement only has to catch what those
  // miss. The old budget put half a metre on every slope steeper than 10%,
  // which is most of a mountain range, and that buys vertex clustering rather
  // than shape: it was 17% of the mesh in triangles under five degrees.
  if (coastDistance(p) < shoreWidth_) return 0.45;
  if (rockAt(p).role != SurfaceRole::Ground) return 0.35;
  const double delta = cellMetres();
  const double slope = std::hypot(linear(terrainElevation_, {p.x+delta,p.y})-linear(terrainElevation_, {p.x-delta,p.y}),
      linear(terrainElevation_, {p.x,p.y+delta})-linear(terrainElevation_, {p.x,p.y-delta}))/(2*delta);
  // A step in the budget draws a band of density change across an otherwise
  // even hillside, so ramp it. Steep ground still refines harder, just not by
  // a factor of five the moment it passes a threshold.
  const double steep = std::clamp((slope - 0.08) / 0.37, 0.0, 1.0);
  double scale = 1.0 - 0.45 * (steep * steep * (3.0 - 2.0 * steep));
  const auto form = static_cast<world::CellType>(landform(p));
  if (form == world::CellType::Mountain || form == world::CellType::Rocks) scale = std::min(scale, 0.45);
  if (form == world::CellType::Valley || linear(riverDistance_, p) < cellMetres() * 2.0) scale = std::min(scale, 0.40);
  // Authored demand on top of measured error. Error refinement answers "where
  // is the surface far from its triangles", which is necessary and not
  // sufficient - a smooth slope is faithfully flat and also dull. This
  // redistributes the budget into patches rather than inflating it.
  return scale * detailField.toleranceScale(p);
}

std::array<float, 3> TerrainMapSource::surfaceColour(Point2 p, const MaterialEntry &material) const
{
  if (material.role != SurfaceRole::Ground) return materialColour(material);
  // Blend cover weights in world metres while retaining semantic face IDs.
  // Geometry normals stay faceted; a biome transition need not be a color wall.
  // A parcel is one material across its polygon, so the cover it was given must
  // survive: substituting the biome here threw the whole allocation away and
  // painted every field the same colour, whatever the palette said.
  // Made boundaries are hard; natural ones are soft.
  //
  // A hedge between two fields is a line somebody planted, and the colour has
  // to change across it exactly - that is what makes a field pattern legible,
  // and blending it was the defect that once painted every field the same. The
  // edge of a wood is not a line: it is where the trees thin out, and drawing it
  // as a hard polygon edge is why woodland reads as flat blobs stamped on the
  // ground rather than as woods.
  //
  // So the rule is by land use, not by geometry. Worked ground - field,
  // orchard, and everything built or made - keeps its exact colour to its own
  // boundary. Ground that is simply what grows there - wood, rough, pasture,
  // meadow - is averaged with its surroundings.
  const uint32_t parcelHere = parcelLayer.cells.empty() ? kNoParcel : parcelLayer.cellAt(p);
  if (parcelHere != kNoParcel)
  {
    const ParcelKind kind = parcelLayer.cells[parcelHere].kind;
    const bool natural = kind == ParcelKind::Woodland || kind == ParcelKind::Rough ||
        kind == ParcelKind::Pasture || kind == ParcelKind::Meadow;
    if (!natural) return materialColour(material);
    // Averaged against the ground around it, so a wood fades into what it
    // borders instead of ending at a polygon edge.
    auto blended = materialColour(material);
    double weight = 1;
    const double radius = std::max(cellMetres() * .8, naturalBlendMetres);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
    {
      if (!x && !y) continue;
      const Point2 q(p.x + x * radius, p.y + y * radius);
      if (waterType(q)) continue;
      const uint32_t other = parcelLayer.cellAt(q);
      MaterialEntry entry = material;
      entry.cover = other == kNoParcel ? static_cast<world::Biome>(biome(q))
          : parcelCover(parcelLayer.cells[other].kind);
      if (other != kNoParcel) entry.tone = parcelLayer.cells[other].tone;
      const auto sample = materialColour(entry);
      const double w = 1.0 / (1 + x * x + y * y);
      for (size_t i = 0; i < 3; ++i) blended[i] += float(sample[i] * w);
      weight += w;
    }
    for (auto &channel : blended) channel = float(channel / weight);
    return blended;
  }
  const auto colorAt = [&](Point2 q)
  {
    MaterialEntry entry = material;
    entry.cover = static_cast<world::Biome>(biome(q));
    if (entry.cover == world::Biome::Shore) entry.cover = world::Biome::Grassland;
    auto color = materialColour(entry);
    const double d = coastDistance(q);
    const double t = std::clamp((d-100.0)/250.0,0.0,1.0);
    MaterialEntry sand = entry; sand.cover = world::Biome::Shore;
    const auto beach = materialColour(sand);
    for (size_t i=0;i<3;++i) color[i] = float(beach[i]*(1-t)+color[i]*t);
    return color;
  };
  auto color = colorAt(p);
  const double altitude = linear(terrainElevation_, p)-seaLevelMetres;
  const bool alpineTransition = std::abs(altitude-2300)<200 || std::abs(altitude-2800)<200;
  if (!bucketBiome_.empty() && bucketBiome_[bucket(p)] != 255 && !alpineTransition) return color;
  double weight = 1;
  const double radius = std::min(175.0,extentMetres*.0015);
  for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x)
  {
    if (!x && !y) continue;
    const Point2 q(p.x+x*radius,p.y+y*radius);
    if (waterType(q)) continue;
    const double w = 1.0/(1+x*x+y*y);
    const auto other = colorAt(q);
    for(size_t i=0;i<3;++i) color[i] += float(other[i]*w);
    weight += w;
  }
  for(auto &c:color)c=float(c/weight);
  return color;
}

world::Biome TerrainMapSource::parcelCover(const ParcelKind kind)
{
  // Tilled ground is ochre, grazed ground is green: that contrast is what makes
  // a field pattern legible, so it lives in the cover rather than in a tone
  // multiplier applied to one green.
  switch (kind)
  {
  case ParcelKind::Field:      return world::Biome::Savanna;
  case ParcelKind::Pasture:    return world::Biome::Grassland;
  case ParcelKind::Meadow:     return world::Biome::Steppe;
  case ParcelKind::Orchard:    return world::Biome::Shrubland;
  case ParcelKind::Woodland:   return world::Biome::TemperateForest;
  case ParcelKind::Settlement: return world::Biome::Shrubland;
  // A croft is green: the contrast against the built plot beside it is what
  // makes a block legible as buildings rather than as one filled polygon.
  case ParcelKind::Yard:       return world::Biome::Grassland;
  // Made ground: pale and dry, so a street reads against the roofs either side.
  case ParcelKind::Street:     return world::Biome::Shore;
  case ParcelKind::Market:     return world::Biome::Steppe;
  case ParcelKind::Citadel:    return world::Biome::ColdDesert;
  // A road is bare made ground, a shore is sand, a river bed is wet gravel.
  // Each needs to read against what it crosses, which is why they are covers of
  // their own rather than a tint on the ground beside them.
  // A roof, against the yard around it.
  case ParcelKind::Building:   return world::Biome::Shrubland;
  case ParcelKind::Road:       return world::Biome::Desert;
  case ParcelKind::Shore:      return world::Biome::Shore;
  case ParcelKind::Riverbank:  return world::Biome::SaltFlat;
  default:                     return world::Biome::Grassland;
  }
}

MaterialFunction TerrainMapSource::materialFunction(MaterialPalette &palette) const
{
  // Every parcel material the layer can ask for, registered up front so the
  // returned closure never mutates the palette - it is called concurrently from
  // every meshing worker.
  std::array<std::array<uint8_t, kParcelToneCount>, uint32_t(ParcelKind::Count)> parcelLookup{};
  if (!parcelLayer.cells.empty())
    for (uint32_t kind = 0; kind < uint32_t(ParcelKind::Count); ++kind)
      for (uint8_t tone = 0; tone < kParcelToneCount; ++tone)
      {
        MaterialEntry entry{};
        entry.role = SurfaceRole::Ground;
        entry.tone = tone;
        entry.cover = parcelCover(ParcelKind(kind));
        parcelLookup[kind][tone] = palette.add(entry);
      }

  std::array<std::array<uint8_t, 256>, 3> lookup{};
  for (uint32_t water = 0; water < lookup.size(); ++water)
    for (uint32_t biome = 0; biome < static_cast<uint32_t>(world::Biome::Count); ++biome)
    {
      MaterialEntry entry{};
      entry.cover = water == 1 ? world::Biome::Ocean : water == 2 ? world::Biome::Lake : static_cast<world::Biome>(biome);
      entry.role = water ? SurfaceRole::Water : SurfaceRole::Ground;
      lookup[water][biome] = palette.add(entry);
    }
  MaterialEntry bed{}; bed.role = SurfaceRole::RiverBed;
  const auto bedMaterial = palette.add(bed);
  std::array<std::array<uint8_t, kPlanetLithologyCount>, 2> rocks{};
  for (uint32_t role=0;role<2;++role) for(uint32_t lith=0;lith<kPlanetLithologyCount;++lith)
  {
    MaterialEntry entry;entry.role=role?SurfaceRole::Talus:SurfaceRole::Face;
    entry.lithology=static_cast<PlanetLithology>(lith);entry.weathering=80;
    rocks[role][lith]=palette.add(entry);
  }
  return [this, lookup, bedMaterial, rocks, parcelLookup](Point2 p)
  {
    const auto water = std::min<uint8_t>(2, waterType(p));
    if (!water && rivers.sample(p).inside) return bedMaterial;
    if(!water)
    {
      const auto rock=rockAt(p);
      if(rock.role==SurfaceRole::Face||rock.role==SurfaceRole::Talus)
      {
        const double d=std::min(30.0,cellMetres()*.25);
        const double slope=std::hypot(height({p.x+d,p.y})-height({p.x-d,p.y}),
            height({p.x,p.y+d})-height({p.x,p.y-d}))/(2*d);
        if(slope>(rock.role==SurfaceRole::Face?.55:.2))
          return rocks[rock.role==SurfaceRole::Talus?1:0][uint32_t(rock.lithology)];
      }
    }
    // A parcel is one material across its whole polygon: that uniformity is
    // what makes a field read as a field rather than as a noise field that
    // happened to change value.
    if (!parcelLayer.cells.empty())
    {
      const uint32_t cell = parcelLayer.cellAt(p);
      // Rough is "nobody works this", not a land use, so it must fall through
      // to whatever the map says the ground is. Answering Grassland for it
      // painted every desert, salt flat, tundra and glacier green wherever a
      // parcel happened to cover them - the allocation was overriding the
      // high-level biome instead of decorating it.
      if (cell != kNoParcel && parcelLayer.cells[cell].kind != ParcelKind::Rough)
      {
        const auto &parcel = parcelLayer.cells[cell];
        return parcelLookup[uint32_t(parcel.kind)][parcel.tone % kParcelToneCount];
      }
    }
    return lookup[water][biome(p)];
  };
}

void TerrainMapSource::flattenOcean(TerrainMesh &mesh) const
{
  RasterTerrain water;
  water.seaLevelMetres = seaLevelMetres;
  flattenRasterOcean(water, mesh);
}

std::vector<Chain> TerrainMapSource::constraintChains() const
{
  std::vector<Chain> chains;
  Chain border{};
  border.kind = ChainKind::WorldBorder; border.closed = true;
  border.points = {{0, 0}, {extentMetres, 0}, {extentMetres, extentMetres}, {0, extentMetres}};
  chains.push_back(border);
  chains.insert(chains.end(),rockFormations.chains().begin(),rockFormations.chains().end());
  const auto &edges = edges_;
  // A region boundary is where the warped query crosses the coarse boundary, so
  // for a coarse point q the world position is q - warp(q), to first order. The
  // chain is resampled first: displacing the endpoints of a 700 m segment would
  // move the coastline without adding any detail to it.
  const auto followWarp = [this](std::vector<Point2> points)
  {
    if (!(regionWarpMetres > 0) || points.size() < 2u) return points;
    const double step = std::max(20.0, regionWarpScaleMetres * .12);
    std::vector<Point2> dense;
    dense.reserve(points.size() * 4u);
    for (size_t i = 0; i + 1 < points.size(); ++i)
    {
      const Point2 a = points[i], b = points[i + 1];
      const double span = std::hypot(b.x - a.x, b.y - a.y);
      const auto pieces = uint32_t(std::max(1.0, std::ceil(span / step)));
      for (uint32_t k = 0; k < pieces; ++k)
      {
        const double t = double(k) / pieces;
        dense.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
      }
    }
    dense.push_back(points.back());
    for (Point2 &q : dense)
    { const Point2 offset = regionWarp(q); q.x -= offset.x; q.y -= offset.y; }
    return dense;
  };
  // Region borders still come from the partition, displaced to follow the warp.
  // The coastline does not: it is the marched contour above, which *is* the
  // land-water boundary rather than an approximation drifting a median 180 m
  // from it.
  for (const auto &edge : edges.edges)
  {
    if (edge.coast) continue;
    Chain chain{}; chain.kind = ChainKind::RegionBorder;
    chain.points = followWarp(edge.path.size() > 1 ? edge.path
        : std::vector<Point2>{edges.vertices[edge.from].position, edges.vertices[edge.to].position});
    chains.push_back(std::move(chain));
  }
  for (const auto &ring : coastLines_)
  {
    if (ring.size() < 3u) continue;
    Chain chain{}; chain.kind = ChainKind::Coast; chain.closed = true;
    chain.points = ring;
    chains.push_back(std::move(chain));
  }
  if (!hasVectorRegions()) for (const auto &line : features_.coastline)
  {
    Chain chain{}; chain.kind = ChainKind::Coast;
    chain.points = followWarp(line.points); chain.closed = line.closed;
    chains.push_back(std::move(chain));
  }
  for (const auto &line : features_.contours.lines)
  {
    // A contour crossing parcelled ground is dropped for the same reason.
    if(!parcelLayer.cells.empty())
    {
      uint32_t inside=0,tested=0;
      for(size_t i=0;i<line.points.size();i+=std::max<size_t>(1u,line.points.size()/12u))
      { ++tested; inside+=parcelLayer.cellAt(line.points[i])!=kNoParcel; }
      if(tested&&inside*2u>tested)continue;
    }
    Chain chain{}; chain.kind = ChainKind::Contour;
    chain.points = line.points; chain.closed = line.closed;
    // These vector contours may predate refined_elevation. Their paths are
    // constraints, but their old levels must not flatten the eroded source.
    chains.push_back(std::move(chain));
  }
  for (const auto &edge : mountains.edges)
  {
    Chain chain{}; chain.kind = ChainKind::Ridge;
    chain.points = {mountains.nodes[edge.from].position, mountains.nodes[edge.to].position};
    chains.push_back(std::move(chain));
  }
  for (const auto &river : features_.rivers.segments)
  {
    Chain chain{}; chain.kind = ChainKind::Stroke;
    chain.points = {river.from, river.to}; chains.push_back(std::move(chain));
  }
  // Parcel boundaries. In parcelled ground these replace the contour and slope
  // guides rather than joining them: a contour inside a flat field describes
  // nothing the field's own edge does not, and two near-parallel chains a few
  // metres apart is the arrangement that makes a ladder of slivers.
  for(const auto &edge:parcelLayer.edges)
  {
    if(edge.path.size()<2u)continue;
    // Only boundaries the mesh has to honour. An edge typed None separates two
    // cells of the same use, block and tone - the material is identical across
    // it, so constraining it buys nothing and costs a triangle fan. Constraining
    // every edge instead took the constraint count from 2.7 M to 6.4 M.
    if(edge.type==ParcelEdgeType::None)continue;
    Chain chain{}; chain.kind=ChainKind::Parcel; chain.points=edge.path;
    chains.push_back(std::move(chain));
  }
  auto wetEdges=rivers.chains();
  chains.insert(chains.end(),std::make_move_iterator(wetEdges.begin()),std::make_move_iterator(wetEdges.end()));
  const auto dry=[this](Point2 p){return p.x>=0&&p.y>=0&&p.x<=extentMetres&&p.y<=extentMetres&&!waterType(p);};
  const HeightFunction surface=[this](Point2 p){return height(p);};
  const double guideSpacing=std::max(50.0,cellMetres()*2);
  if(guidedGeometry)
  {
    for(const auto &edge:mountains.edges)
    {
      const auto &a=mountains.nodes[edge.from],&b=mountains.nodes[edge.to];
      auto guides=ridgeSurfaceGuides(a.position,b.position,.5*(a.widthMetres+b.widthMetres),guideSpacing,surface,dry);
      chains.insert(chains.end(),std::make_move_iterator(guides.begin()),std::make_move_iterator(guides.end()));
    }
    std::vector<CorridorGuide> corridors;corridors.reserve(channels_.size());
    for(const auto &s:channels_)corridors.push_back({s.a,s.b,s.halfWidth,s.outerWidth,s.halfWidthB,s.outerWidth});
    auto rivers=riverSurfaceGuides(corridors,guideSpacing*2);
    chains.insert(chains.end(),std::make_move_iterator(rivers.begin()),std::make_move_iterator(rivers.end()));
    // Slope guides trace steepest descent across open ground. Inside a parcel
    // they describe nothing the parcel's own boundary does not, and running one
    // a few metres from a hedge line is how a ladder of slivers starts. The
    // parcel layer replaces them where it exists.
    auto slopes=slopeSurfaceGuides(extentMetres,guideSpacing,seed,surface,
        [this,&dry](Point2 p){return dry(p)&&parcelLayer.cellAt(p)==kNoParcel;});
    chains.insert(chains.end(),std::make_move_iterator(slopes.begin()),std::make_move_iterator(slopes.end()));
  }
  else
  {
  for (const auto &river : channels_)
  {
    const double dx = river.b.x - river.a.x, dy = river.b.y - river.a.y;
    const double length = std::hypot(dx, dy);
    if (length <= 1e-8) continue;
    const double half = river.halfWidth;
    for (double offset : {-river.outerWidth, -half, 0.0, half, river.outerWidth})
    {
      Chain chain{}; chain.kind = ChainKind::Stroke;
      chain.points = {{river.a.x - dy / length * offset, river.a.y + dx / length * offset},
          {river.b.x - dy / length * offset, river.b.y + dx / length * offset}};
      chains.push_back(std::move(chain));
    }
  }
  }
  // Only exports without vector topology need the raster fallback. Adding
  // pixel boundaries on top of real polygons creates square biome islands.
  if (hasVectorRegions())
  {
    // March the final terrain, including shore and channel processing, rather
    // than the old graph's coarse contour ladder. Crossings interpolate in
    // metres; these contours never follow pixel label edges.
    const uint32_t side = std::min(1024u, resolution);
    const double spacing = extentMetres / side;
    const double interval = 50.0;
    std::vector<double> heights(size_t(side + 1) * (side + 1));
    for (uint32_t y = 0; y <= side; ++y)
      for (uint32_t x = 0; x <= side; ++x)
        heights[size_t(y) * (side + 1) + x] = height({x * spacing, y * spacing});
    const auto march = [&](std::array<Point2, 3> p, std::array<double, 3> h)
    {
      const double low = *std::min_element(h.begin(), h.end());
      const double high = *std::max_element(h.begin(), h.end());
      for (double level = std::max(seaLevelMetres + interval, std::ceil(low / interval) * interval); level < high; level += interval)
      {
        Chain chain{}; chain.kind = ChainKind::Contour;
        for (size_t i = 0; i < 3; ++i)
        {
          const size_t j = (i + 1) % 3;
          if ((h[i] <= level) == (h[j] <= level)) continue;
          const double t = (level - h[i]) / (h[j] - h[i]);
          chain.points.push_back({p[i].x + (p[j].x - p[i].x) * t, p[i].y + (p[j].y - p[i].y) * t});
        }
        if (chain.points.size() == 2) chains.push_back(std::move(chain));
      }
    };
    for (uint32_t y = 0; y < side; ++y)
      for (uint32_t x = 0; x < side; ++x)
      {
        const Point2 a(x * spacing, y * spacing), b((x + 1) * spacing, y * spacing);
        const Point2 c((x + 1) * spacing, (y + 1) * spacing), d(x * spacing, (y + 1) * spacing);
        const auto at = [&](uint32_t xx, uint32_t yy) { return heights[size_t(yy) * (side + 1) + xx]; };
        march({a, b, c}, {at(x, y), at(x + 1, y), at(x + 1, y + 1)});
        march({a, c, d}, {at(x, y), at(x + 1, y + 1), at(x, y + 1)});
      }
    return chains;
  }
  // Preserve the actual raster labels when vector regions are unavailable.
  // Runs are exact cell-edge segments, joined while a boundary continues.
  // Intersections retain label junctions. A half-texel shift here would put
  // a seam inside a biome.
  const auto same = [&](size_t a, size_t b)
  { return biome_[a] == biome_[b] && water_[a] == water_[b] && landform_[a] == landform_[b] && region_[a] == region_[b]; };
  const double cell = cellMetres();
  for (uint32_t axis = 0; axis < 2; ++axis)
    for (uint32_t across = 1; across < resolution; ++across)
    {
      uint32_t start = 0;
      bool running = false;
      for (uint32_t along = 0; along <= resolution; ++along)
      {
        const size_t here = axis == 0 ? size_t(along) * resolution + across : size_t(across) * resolution + along;
        const size_t previous = here - (axis == 0 ? 1 : resolution);
        const bool boundary = along < resolution && !same(here, previous);
        if (boundary && !running) { start = along; running = true; }
        if (!boundary && running)
        {
          Chain chain{}; chain.kind = ChainKind::RegionBorder;
          chain.points = axis == 0 ? std::vector<Point2>{{across * cell, start * cell}, {across * cell, along * cell}}
              : std::vector<Point2>{{start * cell, across * cell}, {along * cell, across * cell}};
          chains.push_back(std::move(chain)); running = false;
        }
      }
    }
  return chains;
}
} // namespace procedural::geometry
