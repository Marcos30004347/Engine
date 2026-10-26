// Native mesh export for the timeline inspection tool.
#pragma once
#include "CityMeshInstances.hpp"
#include "json.hpp"
#include "procedural/world/WorldCityChronicle.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
using namespace procedural::world::rewrite;
using nlohmann::json;

inline json poly(const std::vector<Position> &points)
{
  json out = json::array();
  for (auto p : points)
  {
    out.push_back({p.x, p.y, p.z});
  }
  return out;
}

inline bool writeModel(const CityFabricSite &site, const CityFabricLedger &fabric, const CityDefenceLedger &defence, uint32_t week, const std::filesystem::path &path, std::string &error, CityModelPlan *modelOut = nullptr)
{
  CityFabricGround regenerated;
  if (!planCityBase(site.settings.base, regenerated.base, &error) || !populateCityBase(regenerated.base, regenerated.completion, regenerated.relief, &error))
  {
    return false;
  }
  CityCompletionPlan plan = regenerated.completion;
  const auto historical = fabric.completionAt(site.settlement, week);
  std::map<uint64_t, CityLotDevelopment> developments;
  for (const auto &lot : historical.lots)
  {
    developments[lot.lot] = lot;
  }
  for (auto &lot : plan.lots)
  {
    auto found = developments.find(lot.lot);
    if (found == developments.end())
    {
      error = "regenerated parcel has no semantic record";
      return false;
    }
    lot = found->second;
  }
  plan.routes = historical.routes;

  defence.applyTo(site.settlement, week, plan);

  SettlementFabric ground;

  ground.terrain = regenerated.relief.openGround;

  CityModelSettings options;

  options.groundVertexSpacing = 4;

  // A stair not yet built at this date is not meshed: the relief plan carries
  // every flight the base reserved, the ledger says which ones stand.
  CityReliefPlan relief = regenerated.relief;
  std::vector<Position> unbuilt; // Every point of every access path whose stair is not yet finished.

  for (const auto &[id, s] : site.structures)
  {
    if ((s.kind != CityAccessKind::Stair && s.kind != CityAccessKind::Approach) || (s.completedWeek && *s.completedWeek <= week))
    {
      continue;
    }

    for (const auto &deck : site.ground->relief.platforms)
    {
      if (deck.block == s.block)
      {
        unbuilt.push_back(deck.accessFrom);
        unbuilt.push_back(deck.accessTo);
        unbuilt.insert(unbuilt.end(), deck.accessPath.begin(), deck.accessPath.end());
      }
    }
  }

  auto near = [&](const Position &p) {
    for (const auto &q : unbuilt)
    {
      if (std::hypot(p.x - q.x, p.z - q.z) < 3)
      {
        return true;
      }
    }

    return false;
  };

  relief.stairs.erase(
      std::remove_if(
          relief.stairs.begin(),
          relief.stairs.end(),
          [&](const CityReliefPlan::Stairway &stair) {
            return near(stair.a) || near(stair.b);
          }),
      relief.stairs.end());
  relief.stairLandings.erase(
      std::remove_if(
          relief.stairLandings.begin(),
          relief.stairLandings.end(),
          [&](const CityReliefPlan::StairLanding &landing) {
            for (const auto &p : landing.boundary)
            {
              if (near(p))
              {
                return true;
              }
            }

            return false;
          }),
      relief.stairLandings.end());

  CityModelPlan model;

  if (!buildPreviewCityModel(plan, ground, options, model, &error, &relief))
  {
    return false;
  }

  json j;

  j["sourcePipeline"] = "native-district";
  j["generation"] = {{"mode", "history-regenerated-parcels"}, {"week", week}, {"settlement", site.settlement}};
      json arrays;
      {
        std::ofstream binary(path.parent_path() / (path.stem().string()+".bin"), std::ios::binary);
        binary.exceptions(std::ios::failbit | std::ios::badbit);
        uint64_t at = 0;
        const auto put = [&](const char *name, const void *data, const size_t bytes, const char *type, const json &shape) {
          binary.write(static_cast<const char *>(data), std::streamsize(bytes));
          arrays[name] = {{"offset", at}, {"bytes", bytes}, {"type", type}, {"shape", shape}};
          at += bytes;
        };

        std::vector<double> vertices;
        vertices.reserve(model.mesh.vertices.size() * 3);
        for (const auto &v : model.mesh.vertices) { vertices.push_back(v.x); vertices.push_back(v.y); vertices.push_back(v.z); }
        put("vertices", vertices.data(), vertices.size() * sizeof(double), "f8", json::array({3}));

        std::vector<uint32_t> triangles;
        triangles.reserve(model.mesh.triangles.size() * 3);
        for (const auto &t : model.mesh.triangles) { triangles.push_back(t[0]); triangles.push_back(t[1]); triangles.push_back(t[2]); }
        put("triangles", triangles.data(), triangles.size() * sizeof(uint32_t), "u4", json::array({3}));

        put("materials", model.materials.data(), model.materials.size() * sizeof(uint32_t), "u4", json::array());
        put("surfaceMaterials", model.surfaceMaterials.data(), model.surfaceMaterials.size() * sizeof(uint32_t), "u4", json::array());

        std::vector<float> uvs;
        uvs.reserve(model.triangleUVs.size() * 6);
        for (const auto &corner : model.triangleUVs)
          for (const auto &uv : corner) { uvs.push_back(float(uv[0])); uvs.push_back(float(uv[1])); }
        put("triangleUVs", uvs.data(), uvs.size() * sizeof(float), "f4", json::array({3, 2}));

        std::vector<float> weights;
        weights.reserve(model.groundWeights.size() * 9);
        for (const auto &corner : model.groundWeights)
          for (const auto &weight : corner) for (const double value : weight) weights.push_back(float(value));
        put("weights", weights.data(), weights.size() * sizeof(float), "f4", json::array({3, 3}));
      }

      j["arrays"] = arrays;
      j["arrayFile"] = path.stem().string()+".bin";
  j["groundMaterialIds"] = model.groundMaterialIds;
  j["textureManifest"] = "assets/procedural/textures/medieval-pixel/materials.json";
  j["buildings"] = json::array();

  for (const auto &b : model.buildings)
  {
    j["buildings"].push_back(
        {{"id", b.id},
         {"source", b.source},
         {"kind", cityModelBuildingKindName(b.kind)},
         {"footprint", poly(b.footprint)},
         {"base", b.base},
         {"wallHeight", b.wallHeight},
         {"roofHeight", b.roofHeight},
         {"rooms", b.rooms},
         {"windows", b.windows},
         {"roomConnections", b.roomConnections},
         {"trade", b.trade}, {"architecture",b.architecture},{"architecturalAgeYears",b.architecturalAgeYears},{"weathering",b.weathering},{"colonnadeColumns",b.colonnadeColumns},{"cofferBeams",b.cofferBeams},
         {"exteriorDoors", poly(b.exteriorDoors)},
         {"jettyMetres", b.jettyMetres},
         {"interiorViews", poly(b.interiorViews)}});
    j["buildings"].back()["roomList"] = json::array();
    for (const auto &room : b.roomList)
    {
      j["buildings"].back()["roomList"].push_back({{"id", room.id}, {"floor", room.floor}, {"use", room.use}, {"kind", room.kind}, {"boundary", poly(room.boundary)}, {"area", room.area}});
    }
  }

  j["historicalBindings"] = json::array();
  for (const auto &[id, b] : site.buildings)
  {
    if (!b.active(week))
    {
      continue;
    }
    auto physical = std::find_if(model.buildings.begin(), model.buildings.end(), [&](const auto &m) {
      return m.id == b.parcel;
    });
    j["historicalBindings"].push_back(
        {{"historicalBuilding", id},
         {"parcel", b.parcel},
         {"generatedBuilding", physical == model.buildings.end() ? json(nullptr) : json(physical->id)},
         {"rooms", physical == model.buildings.end() ? 0 : physical->rooms}});
  }
  j["rejections"] = model.rejections;
  j["wallSegments"] = plan.wallSegments.size();
  writeCityInstances(path,model,j);
  if (modelOut != nullptr)
  {
    *modelOut = std::move(model);
  }
  std::ofstream file(path);
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file << j.dump();
  file.close();

  return true;
}
