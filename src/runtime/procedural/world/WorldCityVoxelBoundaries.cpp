#include "WorldCityVoxelBoundaries.hpp"
#include "hierarchy/DwellingVoxelAssembly.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *error, const char *message)
{
  if (error) *error = message;
  return false;
}

int64_t floorDiv(int64_t value, int64_t divisor)
{
  const int64_t quotient = value / divisor, remainder = value % divisor;
  return quotient - (remainder < 0);
}

int32_t tileOfMillimetres(int64_t coordinate)
{
  constexpr int64_t tile = int64_t(CityVoxelTileGrid::millimetresPerVoxel) * CityVoxelTileGrid::voxelsPerTile;
  return static_cast<int32_t>(floorDiv(coordinate, tile));
}

bool quantise(double metres, int64_t &out)
{
  if (!std::isfinite(metres) || std::abs(metres) > 1e10) return false;
  out = std::llround(metres * 1000.);
  return true;
}

CityVoxelFeatureKind featureKind(CitySurfaceKind kind)
{
  switch (kind) {
    case CitySurfaceKind::Road: return CityVoxelFeatureKind::Road;
    case CitySurfaceKind::Sidewalk:
    case CitySurfaceKind::Court:
    case CitySurfaceKind::Landing:
    case CitySurfaceKind::StairTread: return CityVoxelFeatureKind::Paving;
    case CitySurfaceKind::BuildingPad: return CityVoxelFeatureKind::Foundation;
    default: return CityVoxelFeatureKind::Terrain;
  }
}
}

CityVoxelTileAddress cityVoxelTileOfCell(int64_t voxelX, int64_t voxelZ)
{
  return {static_cast<int32_t>(floorDiv(voxelX, CityVoxelTileGrid::voxelsPerTile)),
          static_cast<int32_t>(floorDiv(voxelZ, CityVoxelTileGrid::voxelsPerTile))};
}

bool cityVoxelTerrainSample(int64_t vertexX, int64_t vertexZ,
                            const std::function<double(double,double)> &ground,
                            int64_t &heightMillimetres)
{
  if (!ground) return false;
  constexpr double pitch = double(CityVoxelTileGrid::millimetresPerVoxel) / 1000.;
  return quantise(ground(double(vertexX) * pitch, double(vertexZ) * pitch), heightMillimetres);
}

bool cityVoxelFeature(CityVoxelFeatureKind kind, CityVoxelFallback fallback,
                      uint64_t owner, uint32_t sourceIndex,
                      const std::vector<Position> &ring, CityVoxelFeature &result)
{
  if (ring.size() < 3) return false;
  CityVoxelFeature feature;
  feature.kind = kind; feature.fallback = fallback;
  feature.owner = owner; feature.sourceIndex = sourceIndex;
  feature.boundsMillimetres = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
  for (const auto &point : ring) {
    int64_t x = 0, z = 0;
    if (!quantise(point.x, x) || !quantise(point.z, z)) return false;
    feature.boundsMillimetres[0] = std::min(feature.boundsMillimetres[0], x);
    feature.boundsMillimetres[1] = std::min(feature.boundsMillimetres[1], z);
    feature.boundsMillimetres[2] = std::max(feature.boundsMillimetres[2], x);
    feature.boundsMillimetres[3] = std::max(feature.boundsMillimetres[3], z);
  }
  feature.firstTile = {tileOfMillimetres(feature.boundsMillimetres[0]),
                       tileOfMillimetres(feature.boundsMillimetres[1])};
  feature.lastTile = {tileOfMillimetres(feature.boundsMillimetres[2]),
                      tileOfMillimetres(feature.boundsMillimetres[3])};
  result = feature;
  return true;
}

void addCityVoxelBoundaryFeature(CityVoxelBoundaryPlan &plan, const CityVoxelFeature &feature)
{
  if (plan.features.empty()) {
    plan.terrain.minTileX = feature.firstTile.x;
    plan.terrain.minTileZ = feature.firstTile.z;
    plan.terrain.maxTileX = feature.lastTile.x + 1;
    plan.terrain.maxTileZ = feature.lastTile.z + 1;
  } else {
    plan.terrain.minTileX = std::min(plan.terrain.minTileX, feature.firstTile.x);
    plan.terrain.minTileZ = std::min(plan.terrain.minTileZ, feature.firstTile.z);
    plan.terrain.maxTileX = std::max(plan.terrain.maxTileX, feature.lastTile.x + 1);
    plan.terrain.maxTileZ = std::max(plan.terrain.maxTileZ, feature.lastTile.z + 1);
  }
  plan.features.push_back(feature);
}

bool buildCityVoxelBoundaryPlan(const CitySurfaceStructure &surfaces,
                                const std::vector<CityVoxelPartyLine> &partyLines,
                                const std::set<uint64_t> &acceptedBuildings,
                                CityVoxelBoundaryPlan &result, std::string *error)
{
  CityVoxelBoundaryPlan out;
  out.features.reserve(surfaces.pieces.size());
  for (uint32_t i = 0; i < surfaces.pieces.size(); ++i) {
    const auto &source = surfaces.pieces[i];
    if (source.polygon.outer.size() < 3) continue;
    std::vector<Position> ring;
    ring.reserve(source.polygon.outer.size());
    for (const auto &point : source.polygon.outer)
      ring.push_back({geom2::toMetres(point.x), 0, geom2::toMetres(point.y)});
    CityVoxelFeature feature;
    const auto kind = featureKind(source.kind);
    const auto fallback = kind == CityVoxelFeatureKind::Foundation ? CityVoxelFallback::SolidMesh :
                          kind == CityVoxelFeatureKind::Terrain ? CityVoxelFallback::TerrainHeightfield :
                          CityVoxelFallback::SurfacePolygon;
    if (!cityVoxelFeature(kind, fallback, source.owner, i, ring, feature))
      return fail(error, "Invalid city surface voxel bounds");
    addCityVoxelBoundaryFeature(out, feature);
  }
  if (out.features.empty()) return fail(error, "City voxel terrain has no surface extent");
  out.partySeams.reserve(partyLines.size());
  for (const auto &line : partyLines) {
    if (line.buildingA == 0 || line.buildingB == 0 || line.buildingA == line.buildingB ||
        std::tie(line.edgeMillimetres[0], line.edgeMillimetres[1]) >=
        std::tie(line.edgeMillimetres[2], line.edgeMillimetres[3]))
      return fail(error, "Invalid city voxel party line");
    const bool a = acceptedBuildings.count(line.buildingA), b = acceptedBuildings.count(line.buildingB);
    if (!a && !b) continue;
    out.partySeams.push_back({line.edgeMillimetres, std::min(line.buildingA, line.buildingB),
                              std::max(line.buildingA, line.buildingB),
                              a && b ? std::min(line.buildingA, line.buildingB) :
                                       (a ? line.buildingA : line.buildingB), a && b});
  }
  std::sort(out.partySeams.begin(), out.partySeams.end(), [](const auto &a, const auto &b) {
    return std::tie(a.edgeMillimetres, a.buildingA, a.buildingB) <
           std::tie(b.edgeMillimetres, b.buildingA, b.buildingB);
  });
  out.partySeams.erase(std::unique(out.partySeams.begin(), out.partySeams.end(), [](const auto &a, const auto &b) {
    return a.edgeMillimetres == b.edgeMillimetres && a.buildingA == b.buildingA && a.buildingB == b.buildingB;
  }), out.partySeams.end());
  if (!auditCityVoxelBoundaryPlan(out, error)) return false;
  result = std::move(out);
  if (error) error->clear();
  return true;
}

bool collectCityVoxelPartyFacets(const hierarchy::DwellingVoxelAssembly &assembly,
                                 const hierarchy::Skeleton &skeleton,
                                 const std::vector<CityVoxelPartyLine> &lines,
                                 std::vector<CityVoxelPartyFacet> &result,
                                 std::string *error)
{
  result.clear();
  for (const auto &part : assembly.parts) {
    if (!part.partyWall || part.source != hierarchy::DwellingVoxelPartSource::Piece) continue;
    if (part.sourceIndex >= skeleton.pieces.size())
      return fail(error, "Party wall references a missing skeleton piece");
    const auto &piece = skeleton.pieces[part.sourceIndex];
    if (piece.kind != hierarchy::PieceKind::Wall || piece.outside >= skeleton.structure.faces.size())
      return fail(error, "Party wall has no outside face");
    const auto &face = skeleton.structure.faces[piece.outside];
    if (face.outline.size() != 4) return fail(error, "Party wall face is not a quad");
    const auto &p = face.outline[0], &q = face.outline[1];
    const double wallLength = std::hypot(q.x-p.x,q.z-p.z);
    if (wallLength <= 1e-6) return fail(error, "Degenerate party wall strip");
    for (const auto &line : lines) {
      if (line.buildingA != assembly.dwellingId && line.buildingB != assembly.dwellingId) continue;
      const double ax=double(line.edgeMillimetres[0])/1000.,az=double(line.edgeMillimetres[1])/1000.;
      const double bx=double(line.edgeMillimetres[2])/1000.,bz=double(line.edgeMillimetres[3])/1000.;
      const double dx=bx-ax,dz=bz-az,length=std::hypot(dx,dz);
      if (length<=1e-6) return fail(error, "Degenerate party lot line");
      const double ux=dx/length,uz=dz/length;
      const double parallel=std::abs(((q.x-p.x)*ux+(q.z-p.z)*uz)/wallLength);
      if (parallel<.985) continue;
      const auto station=[&](const rewrite::Position &v) { return (v.x-ax)*ux+(v.z-az)*uz; };
      const auto distance=[&](const rewrite::Position &v) { return (v.x-ax)*(-uz)+(v.z-az)*ux; };
      const double s0=station(p),s1=station(q),d0=distance(p),d1=distance(q);
      const double tolerance=std::max(.1,assembly.grid.voxelMetres);
      if (std::max(std::abs(d0),std::abs(d1))>tolerance ||
          std::max(s0,s1)<-tolerance || std::min(s0,s1)>length+tolerance) continue;
      CityVoxelPartyFacet strip;
      strip.edgeMillimetres=line.edgeMillimetres;
      strip.building=assembly.dwellingId;
      strip.part=part.sourceIndex;
      strip.along0=std::llround(std::clamp(std::min(s0,s1),0.,length)*1000.);
      strip.along1=std::llround(std::clamp(std::max(s0,s1),0.,length)*1000.);
      double bottom=face.outline[0].y,top=bottom;
      for (const auto &vertex:face.outline) {
        bottom=std::min(bottom,vertex.y);top=std::max(top,vertex.y);
      }
      strip.bottom=std::llround((bottom+assembly.grid.baseMetres)*1000.);
      strip.top=std::llround((top+assembly.grid.baseMetres)*1000.);
      strip.offset=std::llround((d0+d1)*500.);
      // A perpendicular corner can be inside the proximity tolerance yet
      // touch the legal seam at only one point. It has no shared face area.
      if (strip.along1<=strip.along0 || strip.top<=strip.bottom) continue;
      result.push_back(strip);
    }
  }
  if (error) error->clear();
  return true;
}

bool resolveCityVoxelPartyContacts(CityVoxelBoundaryPlan &plan,
                                   const std::vector<CityVoxelPartyFacet> &facets,
                                   std::string *error)
{
  using Edge=std::array<int64_t,4>;
  std::map<Edge,std::vector<const CityVoxelPartyFacet *>> byEdge;
  for (const auto &facet:facets) byEdge[facet.edgeMillimetres].push_back(&facet);
  plan.partyContacts.clear();
  for (const auto &seam:plan.partySeams) {
    if (!seam.shared) continue;
    const auto found=byEdge.find(seam.edgeMillimetres);
    if (found==byEdge.end()) continue;
    const auto &strips=found->second;
    for (const auto *a:strips) {
      if (a->building!=seam.buildingA) continue;
      for (const auto *b:strips) {
        if (b->building!=seam.buildingB) continue;
        // A legal lot edge is not proof that two physical wall skins meet.
        // Ten millimetres tolerates source quantization without hiding the
        // faces on opposite sides of a real sub-voxel gap.
        if (std::abs(a->offset-b->offset)>10) continue;
        CityVoxelPartyContact contact;
        contact.edgeMillimetres=seam.edgeMillimetres;
        contact.faceOwner=seam.faceOwner;
        contact.partA=a->part;contact.partB=b->part;
        contact.along0=std::max(a->along0,b->along0);
        contact.along1=std::min(a->along1,b->along1);
        contact.bottom=std::max(a->bottom,b->bottom);
        contact.top=std::min(a->top,b->top);
        if (contact.along1>contact.along0 && contact.top>contact.bottom)
          plan.partyContacts.push_back(contact);
      }
    }
  }
  std::sort(plan.partyContacts.begin(),plan.partyContacts.end(),[](const auto &a,const auto &b) {
    return std::tie(a.edgeMillimetres,a.partA,a.partB,a.along0,a.bottom) <
           std::tie(b.edgeMillimetres,b.partA,b.partB,b.along0,b.bottom);
  });
  if (!auditCityVoxelBoundaryPlan(plan,error)) return false;
  if (error) error->clear();
  return true;
}

bool auditCityVoxelBoundaryPlan(const CityVoxelBoundaryPlan &plan, std::string *error)
{
  if (!plan.lowerTileOwnsBorderFaces || plan.terrain.minTileX >= plan.terrain.maxTileX ||
      plan.terrain.minTileZ >= plan.terrain.maxTileZ)
    return fail(error, "Invalid city voxel tile ownership");
  for (const auto &feature : plan.features)
    if (feature.boundsMillimetres[0] > feature.boundsMillimetres[2] ||
        feature.boundsMillimetres[1] > feature.boundsMillimetres[3] ||
        feature.firstTile.x > feature.lastTile.x || feature.firstTile.z > feature.lastTile.z ||
        feature.firstTile.x < plan.terrain.minTileX || feature.firstTile.z < plan.terrain.minTileZ ||
        feature.lastTile.x >= plan.terrain.maxTileX || feature.lastTile.z >= plan.terrain.maxTileZ)
      return fail(error, "Invalid city voxel feature tile range");
  for (size_t i = 0; i < plan.partySeams.size(); ++i) {
    const auto &seam = plan.partySeams[i];
    if (seam.faceOwner == 0 || (seam.faceOwner != seam.buildingA && seam.faceOwner != seam.buildingB) ||
        (seam.shared && seam.faceOwner != std::min(seam.buildingA, seam.buildingB)) ||
        (i && std::tie(plan.partySeams[i-1].edgeMillimetres, plan.partySeams[i-1].buildingA,
                       plan.partySeams[i-1].buildingB) >=
              std::tie(seam.edgeMillimetres, seam.buildingA, seam.buildingB)))
      return fail(error, "Invalid city voxel party ownership");
  }
  for (const auto &contact:plan.partyContacts) {
    const auto seam=std::find_if(plan.partySeams.begin(),plan.partySeams.end(),[&](const auto &candidate) {
      return candidate.edgeMillimetres==contact.edgeMillimetres && candidate.shared &&
             candidate.faceOwner==contact.faceOwner;
    });
    if (seam==plan.partySeams.end() || contact.along1<=contact.along0 || contact.top<=contact.bottom)
      return fail(error, "Invalid resolved party contact");
  }
  for (size_t i=0;i<plan.partyContacts.size();++i)
    for (size_t j=i+1;j<plan.partyContacts.size() &&
                    plan.partyContacts[j].edgeMillimetres==plan.partyContacts[i].edgeMillimetres;++j) {
      const auto &a=plan.partyContacts[i],&b=plan.partyContacts[j];
      if (std::min(a.along1,b.along1)>std::max(a.along0,b.along0) &&
          std::min(a.top,b.top)>std::max(a.bottom,b.bottom))
        return fail(error, "Overlapping resolved party contacts");
    }
  if (error) error->clear();
  return true;
}

bool sampleCityVoxelFoundationJoin(const std::vector<Position> &footprint, double supportTop,
                                   const std::function<double(double,double)> &ground,
                                   std::vector<CityVoxelFoundationSample> &result,
                                   std::string *error)
{
  int64_t top = 0;
  if (footprint.size() < 3 || !ground || !quantise(supportTop, top))
    return fail(error, "Invalid voxel foundation join input");
  std::map<std::pair<int64_t,int64_t>, CityVoxelFoundationSample> samples;
  for (size_t i = 0; i < footprint.size(); ++i) {
    const auto &a = footprint[i], &b = footprint[(i + 1) % footprint.size()];
    const double length = std::hypot(b.x - a.x, b.z - a.z);
    if (!std::isfinite(length) || length > 1e6) return fail(error, "Invalid voxel foundation edge");
    const uint32_t steps = std::max(1u, static_cast<uint32_t>(std::ceil(length / .1)));
    if (steps>1000000) return fail(error, "Voxel foundation edge exceeds sample budget");
    for (uint32_t j = 0; j < steps; ++j) {
      const double fraction = double(j) / steps;
      int64_t x = 0, z = 0, y = 0;
      if (!quantise(a.x + (b.x - a.x) * fraction, x) ||
          !quantise(a.z + (b.z - a.z) * fraction, z) ||
          !quantise(ground(double(x) / 1000., double(z) / 1000.), y))
        return fail(error, "Nonfinite voxel foundation relief sample");
      if (y > top) return fail(error, "Voxel foundation ground penetrates support top");
      samples[{x,z}] = {x,z,y,top};
    }
  }
  result.clear(); result.reserve(samples.size());
  for (const auto &[key, value] : samples) result.push_back(value);
  if (error) error->clear();
  return true;
}

} // namespace procedural::world::rewrite
